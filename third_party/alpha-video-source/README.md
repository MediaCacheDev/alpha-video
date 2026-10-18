# alpha-video-source (native)

`src/` is `native/src` from [alpha-video-source](https://github.com/MediaCacheDev/alpha-video-source), unmodified, at commit `2c7300f0b04ddb6364409d87f4c99c869a47a182`. MIT, by the same author as this repository.

It reads a file by range from an upstream (the platform's HTTP fetcher or an app's data source), through blocks in memory and an optional store on disk or in memory. The players' `cpp/ByteSource` reads through it for URLs and data sources. Its sources compile with the core's, with `src/` on the include path.

To update, run `scripts/vendor_alpha_video_source.sh` against a newer checkout. Its tests live in that repository (`native/test`).
