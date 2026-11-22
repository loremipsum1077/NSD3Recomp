# replace

Code the game needs that the decomp does not provide yet. Unlike the rest of the
platform layer, nothing here replaces the Switch: it fills gaps in the decomp, and
each file should disappear once the decomp has the real thing. When that happens
the link reports a symbol defined twice, which is the cue to delete it here.

- `unimplemented.c`: functions our build calls that have no definition anywhere.
  Each stops the program and prints its name.
- `protobuf_field_skipper.cc`: two small protobuf classes, written from protobuf
  2.6.0's public behaviour.
