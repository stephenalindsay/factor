#include "master.hpp"

/* used during garbage collection only */
F_ZONE *newspace;
bool performing_gc;
bool performing_compaction;
CELL collecting_gen;

/* if true, we collecting AGING space for the second time, so if it is still
full, we go on to collect TENURED */
bool collecting_aging_again;

/* in case a generation fills up in the middle of a gc, we jump back
up to try collecting the next generation. */
jmp_buf gc_jmp;

F_GC_STATS gc_stats[MAX_GEN_COUNT];
u64 cards_scanned;
u64 decks_scanned;
u64 card_scan_time;
CELL code_heap_scans;

/* What generation was being collected when copy_code_heap_roots() was last
called? Until the next call to add_code_block(), future
collections of younger generations don't have to touch the code
heap. */
CELL last_code_heap_scan;

/* sometimes we grow the heap */
bool growing_data_heap;
F_DATA_HEAP *old_data_heap;

void init_data_gc(void)
{
	performing_gc = false;
	last_code_heap_scan = NURSERY;
	collecting_aging_again = false;
}

/* Given a pointer to oldspace, copy it to newspace */
static F_OBJECT *copy_untagged_object_impl(F_OBJECT *pointer, CELL size)
{
	if(newspace->here + size >= newspace->end)
		longjmp(gc_jmp,1);
	F_OBJECT *newpointer = allot_zone(newspace,size);

	F_GC_STATS *s = &gc_stats[collecting_gen];
	s->object_count++;
	s->bytes_copied += size;

	memcpy(newpointer,pointer,size);
	return newpointer;
}

static F_OBJECT *copy_object_impl(F_OBJECT *untagged)
{
	F_OBJECT *newpointer = copy_untagged_object_impl(untagged,untagged_object_size(untagged));
	untagged->header.forward_to(newpointer);
	return newpointer;
}

static bool should_copy_p(F_OBJECT *untagged)
{
	if(in_zone(newspace,untagged))
		return false;
	if(collecting_gen == TENURED)
		return true;
	else if(HAVE_AGING_P && collecting_gen == AGING)
		return !in_zone(&data_heap->generations[TENURED],untagged);
	else if(collecting_gen == NURSERY)
		return in_zone(&nursery,untagged);
	else
	{
		critical_error("Bug in should_copy_p",(CELL)untagged);
		return false;
	}
}

/* Follow a chain of forwarding pointers */
static F_OBJECT *resolve_forwarding(F_OBJECT *untagged)
{
	check_data_pointer(untagged);

	/* is there another forwarding pointer? */
	if(untagged->header.forwarding_pointer_p())
		return resolve_forwarding(untagged->header.forwarding_pointer());
	/* we've found the destination */
	else
	{
		untagged->header.check_header();
		if(should_copy_p(untagged))
			return copy_object_impl(untagged);
		else
			return untagged;
	}
}

template <typename T> static T *copy_untagged_object(T *untagged)
{
	check_data_pointer(untagged);

	if(untagged->header.forwarding_pointer_p())
		untagged = (T *)resolve_forwarding(untagged->header.forwarding_pointer());
	else
	{
		untagged->header.check_header();
		untagged = (T *)copy_object_impl(untagged);
	}

	return untagged;
}

static CELL copy_object(CELL pointer)
{
	return RETAG(copy_untagged_object(untag<F_OBJECT>(pointer)),TAG(pointer));
}

void copy_handle(CELL *handle)
{
	CELL pointer = *handle;

	if(!immediate_p(pointer))
	{
		F_OBJECT *object = untag<F_OBJECT>(pointer);
		check_data_pointer(object);
		if(should_copy_p(object))
			*handle = copy_object(pointer);
	}
}

/* Scan all the objects in the card */
static void copy_card(F_CARD *ptr, CELL gen, CELL here)
{
	CELL card_scan = (CELL)CARD_TO_ADDR(ptr) + CARD_OFFSET(ptr);
	CELL card_end = (CELL)CARD_TO_ADDR(ptr + 1);

	if(here < card_end)
		card_end = here;

	copy_reachable_objects(card_scan,&card_end);

	cards_scanned++;
}

static void copy_card_deck(F_DECK *deck, CELL gen, F_CARD mask, F_CARD unmask)
{
	F_CARD *first_card = DECK_TO_CARD(deck);
	F_CARD *last_card = DECK_TO_CARD(deck + 1);

	CELL here = data_heap->generations[gen].here;

	u32 *quad_ptr;
	u32 quad_mask = mask | (mask << 8) | (mask << 16) | (mask << 24);

	for(quad_ptr = (u32 *)first_card; quad_ptr < (u32 *)last_card; quad_ptr++)
	{
		if(*quad_ptr & quad_mask)
		{
			F_CARD *ptr = (F_CARD *)quad_ptr;

			int card;
			for(card = 0; card < 4; card++)
			{
				if(ptr[card] & mask)
				{
					copy_card(&ptr[card],gen,here);
					ptr[card] &= ~unmask;
				}
			}
		}
	}

	decks_scanned++;
}

/* Copy all newspace objects referenced from marked cards to the destination */
static void copy_gen_cards(CELL gen)
{
	F_DECK *first_deck = ADDR_TO_DECK(data_heap->generations[gen].start);
	F_DECK *last_deck = ADDR_TO_DECK(data_heap->generations[gen].end);

	F_CARD mask, unmask;

	/* if we are collecting the nursery, we care about old->nursery pointers
	but not old->aging pointers */
	if(collecting_gen == NURSERY)
	{
		mask = CARD_POINTS_TO_NURSERY;

		/* after the collection, no old->nursery pointers remain
		anywhere, but old->aging pointers might remain in tenured
		space */
		if(gen == TENURED)
			unmask = CARD_POINTS_TO_NURSERY;
		/* after the collection, all cards in aging space can be
		cleared */
		else if(HAVE_AGING_P && gen == AGING)
			unmask = CARD_MARK_MASK;
		else
		{
			critical_error("bug in copy_gen_cards",gen);
			return;
		}
	}
	/* if we are collecting aging space into tenured space, we care about
	all old->nursery and old->aging pointers. no old->aging pointers can
	remain */
	else if(HAVE_AGING_P && collecting_gen == AGING)
	{
		if(collecting_aging_again)
		{
			mask = CARD_POINTS_TO_AGING;
			unmask = CARD_MARK_MASK;
		}
		/* after we collect aging space into the aging semispace, no
		old->nursery pointers remain but tenured space might still have
		pointers to aging space. */
		else
		{
			mask = CARD_POINTS_TO_AGING;
			unmask = CARD_POINTS_TO_NURSERY;
		}
	}
	else
	{
		critical_error("bug in copy_gen_cards",gen);
		return;
	}

	F_DECK *ptr;

	for(ptr = first_deck; ptr < last_deck; ptr++)
	{
		if(*ptr & mask)
		{
			copy_card_deck(ptr,gen,mask,unmask);
			*ptr &= ~unmask;
		}
	}
}

/* Scan cards in all generations older than the one being collected, copying
old->new references */
static void copy_cards(void)
{
	u64 start = current_micros();

	CELL i;
	for(i = collecting_gen + 1; i < data_heap->gen_count; i++)
		copy_gen_cards(i);

	card_scan_time += (current_micros() - start);
}

/* Copy all tagged pointers in a range of memory */
static void copy_stack_elements(F_SEGMENT *region, CELL top)
{
	CELL ptr = region->start;

	for(; ptr <= top; ptr += CELLS)
		copy_handle((CELL*)ptr);
}

static void copy_registered_locals(void)
{
	CELL scan = gc_locals_region->start;

	for(; scan <= gc_locals; scan += CELLS)
		copy_handle(*(CELL **)scan);
}

static void copy_registered_bignums(void)
{
	CELL scan = gc_bignums_region->start;

	for(; scan <= gc_bignums; scan += CELLS)
	{
		F_BIGNUM **handle = *(F_BIGNUM ***)scan;
		F_BIGNUM *pointer = *handle;

		if(pointer)
		{
			check_data_pointer(pointer);
			if(should_copy_p(pointer))
				*handle = copy_untagged_object(pointer);
#ifdef FACTOR_DEBUG
			assert((*handle)->header.hi_tag() == BIGNUM_TYPE);
#endif
		}
	}
}

/* Copy roots over at the start of GC, namely various constants, stacks,
the user environment and extra roots registered by local_roots.hpp */
static void copy_roots(void)
{
	copy_handle(&T);
	copy_handle(&bignum_zero);
	copy_handle(&bignum_pos_one);
	copy_handle(&bignum_neg_one);

	copy_registered_locals();
	copy_registered_bignums();

	if(!performing_compaction)
	{
		save_stacks();
		F_CONTEXT *stacks = stack_chain;

		while(stacks)
		{
			copy_stack_elements(stacks->datastack_region,stacks->datastack);
			copy_stack_elements(stacks->retainstack_region,stacks->retainstack);

			copy_handle(&stacks->catchstack_save);
			copy_handle(&stacks->current_callback_save);

			mark_active_blocks(stacks);

			stacks = stacks->next;
		}
	}

	int i;
	for(i = 0; i < USER_ENV; i++)
		copy_handle(&userenv[i]);
}

static CELL copy_next_from_nursery(CELL scan)
{
	CELL *obj = (CELL *)scan;
	CELL *end = (CELL *)(scan + binary_payload_start((F_OBJECT *)scan));

	if(obj != end)
	{
		obj++;

		CELL nursery_start = nursery.start;
		CELL nursery_end = nursery.end;

		for(; obj < end; obj++)
		{
			CELL pointer = *obj;

			if(!immediate_p(pointer))
			{
				check_data_pointer((F_OBJECT *)pointer);
				if(pointer >= nursery_start && pointer < nursery_end)
					*obj = copy_object(pointer);
			}
		}
	}

	return scan + untagged_object_size((F_OBJECT *)scan);
}

static CELL copy_next_from_aging(CELL scan)
{
	CELL *obj = (CELL *)scan;
	CELL *end = (CELL *)(scan + binary_payload_start((F_OBJECT *)scan));

	if(obj != end)
	{
		obj++;

		CELL tenured_start = data_heap->generations[TENURED].start;
		CELL tenured_end = data_heap->generations[TENURED].end;

		CELL newspace_start = newspace->start;
		CELL newspace_end = newspace->end;

		for(; obj < end; obj++)
		{
			CELL pointer = *obj;

			if(!immediate_p(pointer))
			{
				check_data_pointer((F_OBJECT *)pointer);
				if(!(pointer >= newspace_start && pointer < newspace_end)
				   && !(pointer >= tenured_start && pointer < tenured_end))
					*obj = copy_object(pointer);
			}
		}
	}

	return scan + untagged_object_size((F_OBJECT *)scan);
}

static CELL copy_next_from_tenured(CELL scan)
{
	CELL *obj = (CELL *)scan;
	CELL *end = (CELL *)(scan + binary_payload_start((F_OBJECT *)scan));

	if(obj != end)
	{
		obj++;

		CELL newspace_start = newspace->start;
		CELL newspace_end = newspace->end;

		for(; obj < end; obj++)
		{
			CELL pointer = *obj;

			if(!immediate_p(pointer))
			{
				check_data_pointer((F_OBJECT *)pointer);
				if(!(pointer >= newspace_start && pointer < newspace_end))
					*obj = copy_object(pointer);
			}
		}
	}

	mark_object_code_block((F_OBJECT *)scan);

	return scan + untagged_object_size((F_OBJECT *)scan);
}

void copy_reachable_objects(CELL scan, CELL *end)
{
	if(collecting_gen == NURSERY)
	{
		while(scan < *end)
			scan = copy_next_from_nursery(scan);
	}
	else if(HAVE_AGING_P && collecting_gen == AGING)
	{
		while(scan < *end)
			scan = copy_next_from_aging(scan);
	}
	else if(collecting_gen == TENURED)
	{
		while(scan < *end)
			scan = copy_next_from_tenured(scan);
	}
}

/* Prepare to start copying reachable objects into an unused zone */
static void begin_gc(CELL requested_bytes)
{
	if(growing_data_heap)
	{
		if(collecting_gen != TENURED)
			critical_error("Invalid parameters to begin_gc",0);

		old_data_heap = data_heap;
		set_data_heap(grow_data_heap(old_data_heap,requested_bytes));
		newspace = &data_heap->generations[TENURED];
	}
	else if(collecting_accumulation_gen_p())
	{
		/* when collecting one of these generations, rotate it
		with the semispace */
		F_ZONE z = data_heap->generations[collecting_gen];
		data_heap->generations[collecting_gen] = data_heap->semispaces[collecting_gen];
		data_heap->semispaces[collecting_gen] = z;
		reset_generation(collecting_gen);
		newspace = &data_heap->generations[collecting_gen];
		clear_cards(collecting_gen,collecting_gen);
		clear_decks(collecting_gen,collecting_gen);
		clear_allot_markers(collecting_gen,collecting_gen);
	}
	else
	{
		/* when collecting a younger generation, we copy
		reachable objects to the next oldest generation,
		so we set the newspace so the next generation. */
		newspace = &data_heap->generations[collecting_gen + 1];
	}
}

static void end_gc(CELL gc_elapsed)
{
	F_GC_STATS *s = &gc_stats[collecting_gen];

	s->collections++;
	s->gc_time += gc_elapsed;
	if(s->max_gc_time < gc_elapsed)
		s->max_gc_time = gc_elapsed;

	if(growing_data_heap)
	{
		dealloc_data_heap(old_data_heap);
		old_data_heap = NULL;
		growing_data_heap = false;
	}

	if(collecting_accumulation_gen_p())
	{
		/* all younger generations except are now empty.
		if collecting_gen == NURSERY here, we only have 1 generation;
		old-school Cheney collector */
		if(collecting_gen != NURSERY)
			reset_generations(NURSERY,collecting_gen - 1);
	}
	else if(collecting_gen == NURSERY)
	{
		nursery.here = nursery.start;
	}
	else
	{
		/* all generations up to and including the one
		collected are now empty */
		reset_generations(NURSERY,collecting_gen);
	}

	collecting_aging_again = false;
}

/* Collect gen and all younger generations.
If growing_data_heap_ is true, we must grow the data heap to such a size that
an allocation of requested_bytes won't fail */
void garbage_collection(CELL gen,
	bool growing_data_heap_,
	CELL requested_bytes)
{
	if(gc_off)
	{
		critical_error("GC disabled",gen);
		return;
	}

	u64 start = current_micros();

	performing_gc = true;
	growing_data_heap = growing_data_heap_;
	collecting_gen = gen;

	/* we come back here if a generation is full */
	if(setjmp(gc_jmp))
	{
		/* We have no older generations we can try collecting, so we
		resort to growing the data heap */
		if(collecting_gen == TENURED)
		{
			growing_data_heap = true;

			/* see the comment in unmark_marked() */
			unmark_marked(&code_heap);
		}
		/* we try collecting AGING space twice before going on to
		collect TENURED */
		else if(HAVE_AGING_P
			&& collecting_gen == AGING
			&& !collecting_aging_again)
		{
			collecting_aging_again = true;
		}
		/* Collect the next oldest generation */
		else
		{
			collecting_gen++;
		}
	}

	begin_gc(requested_bytes);

	/* initialize chase pointer */
	CELL scan = newspace->here;

	/* collect objects referenced from stacks and environment */
	copy_roots();
	/* collect objects referenced from older generations */
	copy_cards();

	/* do some tracing */
	copy_reachable_objects(scan,&newspace->here);

	/* don't scan code heap unless it has pointers to this
	generation or younger */
	if(collecting_gen >= last_code_heap_scan)
	{
		code_heap_scans++;

		if(collecting_gen == TENURED)
			free_unmarked(&code_heap,(HEAP_ITERATOR)update_literal_and_word_references);
		else
			copy_code_heap_roots();

		if(collecting_accumulation_gen_p())
			last_code_heap_scan = collecting_gen;
		else
			last_code_heap_scan = collecting_gen + 1;
	}

	CELL gc_elapsed = (current_micros() - start);

	end_gc(gc_elapsed);

	performing_gc = false;
}

void gc(void)
{
	garbage_collection(TENURED,false,0);
}

PRIMITIVE(gc)
{
	gc();
}

PRIMITIVE(gc_stats)
{
	growable_array stats;

	CELL i;
	u64 total_gc_time = 0;

	for(i = 0; i < MAX_GEN_COUNT; i++)
	{
		F_GC_STATS *s = &gc_stats[i];
		stats.add(allot_cell(s->collections));
		stats.add(tag<F_BIGNUM>(long_long_to_bignum(s->gc_time)));
		stats.add(tag<F_BIGNUM>(long_long_to_bignum(s->max_gc_time)));
		stats.add(allot_cell(s->collections == 0 ? 0 : s->gc_time / s->collections));
		stats.add(allot_cell(s->object_count));
		stats.add(tag<F_BIGNUM>(long_long_to_bignum(s->bytes_copied)));

		total_gc_time += s->gc_time;
	}

	stats.add(tag<F_BIGNUM>(ulong_long_to_bignum(total_gc_time)));
	stats.add(tag<F_BIGNUM>(ulong_long_to_bignum(cards_scanned)));
	stats.add(tag<F_BIGNUM>(ulong_long_to_bignum(decks_scanned)));
	stats.add(tag<F_BIGNUM>(ulong_long_to_bignum(card_scan_time)));
	stats.add(allot_cell(code_heap_scans));

	stats.trim();
	dpush(stats.array.value());
}

void clear_gc_stats(void)
{
	int i;
	for(i = 0; i < MAX_GEN_COUNT; i++)
		memset(&gc_stats[i],0,sizeof(F_GC_STATS));

	cards_scanned = 0;
	decks_scanned = 0;
	card_scan_time = 0;
	code_heap_scans = 0;
}

PRIMITIVE(clear_gc_stats)
{
	clear_gc_stats();
}

/* classes.tuple uses this to reshape tuples; tools.deploy.shaker uses this
   to coalesce equal but distinct quotations and wrappers. */
PRIMITIVE(become)
{
	F_ARRAY *new_objects = untag_check<F_ARRAY>(dpop());
	F_ARRAY *old_objects = untag_check<F_ARRAY>(dpop());

	CELL capacity = array_capacity(new_objects);
	if(capacity != array_capacity(old_objects))
		critical_error("bad parameters to become",0);

	CELL i;

	for(i = 0; i < capacity; i++)
	{
		tagged<F_OBJECT> old_obj(array_nth(old_objects,i));
		tagged<F_OBJECT> new_obj(array_nth(new_objects,i));

		if(old_obj != new_obj)
			old_obj->header.forward_to(new_obj.untagged());
	}

	gc();

	/* If a word's definition quotation was in old_objects and the
	   quotation in new_objects is not compiled, we might leak memory
	   by referencing the old quotation unless we recompile all
	   unoptimized words. */
	compile_all_words();
}

VM_C_API void minor_gc(void)
{
	garbage_collection(NURSERY,false,0);
}