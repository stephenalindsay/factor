USING: alien alien.libraries alien.syntax tools.test kernel ;
IN: alien.libraries.tests

[ f ] [ DLL" fadfasdfsada" dll-valid? ] unit-test

[ f ] [ "does not exist" DLL" fadsfasfdsaf" dlsym ] unit-test

[ ] [ "doesnotexist" dlopen dlclose ] unit-test

[ "fdasfsf" dll-valid? drop ] must-fail

[ t ] [
    "test-library" "blah" cdecl add-library
    "test-library" "BLAH" cdecl add-library?
    "blah" remove-library
] unit-test

[ t ] [
    "test-library" "blah" cdecl add-library
    "test-library" "blah" stdcall add-library?
    "blah" remove-library
] unit-test

[ f ] [
    "test-library" "blah" cdecl add-library
    "test-library" "blah" cdecl add-library?
    "blah" remove-library
] unit-test
