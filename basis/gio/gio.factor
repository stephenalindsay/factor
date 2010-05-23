! Copyright (C) 2009 Anton Gorenko.
! See http://factorcode.org/license.txt for BSD license.
USING: alien alien.libraries combinators kernel system
gir glib gobject ;
EXCLUDE: alien.c-types => pointer ;

<<
"gio" {
    { [ os winnt? ] [ "libgio-2.0-0.dll" cdecl add-library ] }
    { [ os macosx? ] [ drop ] }
    { [ os unix? ] [ "libgio-2.0.so" cdecl add-library ] }
} cond
>>

IN-GIR: gio vocab:gio/Gio-2.0.gir

