#include "master.hpp"

F_CONTEXT *stack_chain;
CELL ds_size, rs_size;
F_CONTEXT *unused_contexts;

void reset_datastack(void)
{
	ds = ds_bot - CELLS;
}

void reset_retainstack(void)
{
	rs = rs_bot - CELLS;
}

#define RESERVED (64 * CELLS)

void fix_stacks(void)
{
	if(ds + CELLS < ds_bot || ds + RESERVED >= ds_top) reset_datastack();
	if(rs + CELLS < rs_bot || rs + RESERVED >= rs_top) reset_retainstack();
}

/* called before entry into foreign C code. Note that ds and rs might
be stored in registers, so callbacks must save and restore the correct values */
void save_stacks(void)
{
	if(stack_chain)
	{
		stack_chain->datastack = ds;
		stack_chain->retainstack = rs;
	}
}

F_CONTEXT *alloc_context(void)
{
	F_CONTEXT *context;

	if(unused_contexts)
	{
		context = unused_contexts;
		unused_contexts = unused_contexts->next;
	}
	else
	{
		context = (F_CONTEXT *)safe_malloc(sizeof(F_CONTEXT));
		context->datastack_region = alloc_segment(ds_size);
		context->retainstack_region = alloc_segment(rs_size);
	}

	return context;
}

void dealloc_context(F_CONTEXT *context)
{
	context->next = unused_contexts;
	unused_contexts = context;
}

/* called on entry into a compiled callback */
void nest_stacks(void)
{
	F_CONTEXT *new_stacks = alloc_context();

	new_stacks->callstack_bottom = (F_STACK_FRAME *)-1;
	new_stacks->callstack_top = (F_STACK_FRAME *)-1;

	/* note that these register values are not necessarily valid stack
	pointers. they are merely saved non-volatile registers, and are
	restored in unnest_stacks(). consider this scenario:
	- factor code calls C function
	- C function saves ds/cs registers (since they're non-volatile)
	- C function clobbers them
	- C function calls Factor callback
	- Factor callback returns
	- C function restores registers
	- C function returns to Factor code */
	new_stacks->datastack_save = ds;
	new_stacks->retainstack_save = rs;

	/* save per-callback userenv */
	new_stacks->current_callback_save = userenv[CURRENT_CALLBACK_ENV];
	new_stacks->catchstack_save = userenv[CATCHSTACK_ENV];

	new_stacks->next = stack_chain;
	stack_chain = new_stacks;

	reset_datastack();
	reset_retainstack();
}

/* called when leaving a compiled callback */
void unnest_stacks(void)
{
	ds = stack_chain->datastack_save;
	rs = stack_chain->retainstack_save;

	/* restore per-callback userenv */
	userenv[CURRENT_CALLBACK_ENV] = stack_chain->current_callback_save;
	userenv[CATCHSTACK_ENV] = stack_chain->catchstack_save;

	F_CONTEXT *old_stacks = stack_chain;
	stack_chain = old_stacks->next;
	dealloc_context(old_stacks);
}

/* called on startup */
void init_stacks(CELL ds_size_, CELL rs_size_)
{
	ds_size = ds_size_;
	rs_size = rs_size_;
	stack_chain = NULL;
	unused_contexts = NULL;
}

bool stack_to_array(CELL bottom, CELL top)
{
	F_FIXNUM depth = (F_FIXNUM)(top - bottom + CELLS);

	if(depth < 0)
		return false;
	else
	{
		F_ARRAY *a = allot_array_internal<F_ARRAY>(depth / CELLS);
		memcpy(a + 1,(void*)bottom,depth);
		dpush(tag<F_ARRAY>(a));
		return true;
	}
}

PRIMITIVE(datastack)
{
	if(!stack_to_array(ds_bot,ds))
		general_error(ERROR_DS_UNDERFLOW,F,F,NULL);
}

PRIMITIVE(retainstack)
{
	if(!stack_to_array(rs_bot,rs))
		general_error(ERROR_RS_UNDERFLOW,F,F,NULL);
}

/* returns pointer to top of stack */
CELL array_to_stack(F_ARRAY *array, CELL bottom)
{
	CELL depth = array_capacity(array) * CELLS;
	memcpy((void*)bottom,array + 1,depth);
	return bottom + depth - CELLS;
}

PRIMITIVE(set_datastack)
{
	ds = array_to_stack(untag_check<F_ARRAY>(dpop()),ds_bot);
}

PRIMITIVE(set_retainstack)
{
	rs = array_to_stack(untag_check<F_ARRAY>(dpop()),rs_bot);
}

/* Used to implement call( */
PRIMITIVE(check_datastack)
{
	F_FIXNUM out = to_fixnum(dpop());
	F_FIXNUM in = to_fixnum(dpop());
	F_FIXNUM height = out - in;
	F_ARRAY *array = untag_check<F_ARRAY>(dpop());
	F_FIXNUM length = array_capacity(array);
	F_FIXNUM depth = (ds - ds_bot + CELLS) / CELLS;
	if(depth - height != length)
		dpush(F);
	else
	{
		F_FIXNUM i;
		for(i = 0; i < length - in; i++)
		{
			if(((CELL *)ds_bot)[i] != array_nth(array,i))
			{
				dpush(F);
				return;
			}
		}
		dpush(T);
	}
}