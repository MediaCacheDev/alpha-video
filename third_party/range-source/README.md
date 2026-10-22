# range-source (native)

`include/` and `src/` are `native/include` and `native/src` from [range-source](https://github.com/MediaCacheDev/range-source), unmodified, at commit `2479763355aa201655184025a65b7c6c5b8210f2`. MIT, by the same author as this repository.

- `include/range_source.h` is the add-on's C interface, a table of functions, with a C++ wrapper over it. The player reads URLs and data sources through it (`cpp/RangeSource.h`).
- `include/range_source_download.h` is the basic input, for when the add-on isn't there: a URL downloaded whole into a temporary file, read as it arrives.
- `src/` is the add-on's reader and stores, compiled in until the add-on is its own package.

To update, run `scripts/vendor_range_source.sh` against a newer checkout. Its tests live in that repository (`native/test`).
