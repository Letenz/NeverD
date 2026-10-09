# Write the bytes of INPUT to OUTPUT as the elements of a C array
# initializer, eight to a line (CMake regular expressions have no counted
# repetition), so a source file can embed a data file without
# reading it at run time.
#
#   cmake -DINPUT=<file> -DOUTPUT=<file.inc> -P NeverDEmbedBytes.cmake
file(READ "${INPUT}" _hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],))" "\\1\n" _bytes "${_bytes}")
file(WRITE "${OUTPUT}" "${_bytes}\n")
