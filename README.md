```
Usage: h2cpp [options] header.h

Writes stub definitions for every function declared in a C++ header.
Non-void functions return a value-initialised result ({}), or a
reference to a static default object for reference return types.

Options:
  -o, --output FILE       write to FILE instead of stdout. If FILE exists,
                          only stubs it lacks are added, in header order
  -d, --dry-run           with -o, change nothing: print to stdout what
                          would be written or added, and where
  -t, --trailing-return   use 'auto f() -> T' for every member function
                          (helps when return types are inherited nested types)
  -i, --ignore MACRO      treat MACRO (and any (...) after it) as noise,
                          e.g. -i MYLIB_EXPORT; may be repeated
  -h, --help              show this help
```
