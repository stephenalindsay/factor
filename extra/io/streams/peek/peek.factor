! Copyright (C) 2011 Doug Coleman.
! See http://factorcode.org/license.txt for BSD license.
USING: accessors combinators combinators.short-circuit
destructors io io.private kernel locals math sequences
vectors ;
IN: io.streams.peek

TUPLE: peek-stream stream peeked ;
INSTANCE: peek-stream input-stream
INSTANCE: peek-stream output-stream

M: peek-stream dispose stream>> dispose ;

: stream-new-resizable ( n stream -- exemplar )
    stream-exemplar new-resizable ; inline

: stream-like ( sequence stream -- sequence' )
    stream-exemplar like ; inline

: stream-clone-resizable ( sequence stream -- sequence' )
    stream-exemplar-growable clone-like ; inline

: <peek-stream> ( stream -- stream )
    peek-stream new
        swap >>stream
        64 over stream-new-resizable >>peeked ; inline

M: peek-stream stream-element-type
    stream>> stream-element-type ;

M: peek-stream stream-read1
    dup peeked>> [
        stream>> stream-read1
    ] [
        pop nip
    ] if-empty ;

M:: peek-stream stream-read-unsafe ( n buf stream -- count )
    stream peeked>> :> peeked
    peeked length :> #peeked
    #peeked 0 = [
        n buf stream stream>> stream-read-unsafe
    ] [
        #peeked n >= [
            peeked <reversed> n head-slice 0 buf copy
            peeked [ length n - ] keep shorten
            n
        ] [
            peeked <reversed> 0 buf copy
            0 peeked shorten
            n #peeked - :> n'
            buf #peeked tail-slice :> buf'
            n' buf' stream stream-read-unsafe #peeked +
        ] if
    ] if ;

: peek-stream-read-until ( stream seps buf -- stream seps buf sep/f )
    3dup [ [ stream-read1 dup ] dip member-eq? ] dip swap
    [ drop ] [ over [ push peek-stream-read-until ] [ drop ] if ] if ;

M: peek-stream stream-read-until
    swap 64 pick stream-new-resizable
    peek-stream-read-until [ nip swap stream-like ] dip ;

M: peek-stream stream-write stream>> stream-write ;
M: peek-stream stream-write1 stream>> stream-write1 ;
M: peek-stream stream-flush stream>> stream-flush ;

: stream-peek1 ( stream -- ch )
    dup peeked>> [
        dup stream>> stream-read1 [
            [ 1vector over stream-clone-resizable >>peeked drop ] keep
        ] [
            drop f
        ] if*
    ] [
        last nip
    ] if-empty ;

: stream-peek ( n stream -- seq )
    2dup peeked>> { [ length <= ] [ length 0 > ] } 1&& [
        [ peeked>> <reversed> swap head ] [ stream-exemplar like ] bi
    ] [
        [ nip ]
        [ stream-read ] 2bi
        [ reverse swap peeked>> push-all ] keep
    ] if ;
