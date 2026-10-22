| Run | FFmpeg reads | MB read | Disk | Block | Policy | Total s | Waiting s | Blocks | Wasted |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| chunk-copy-mkv | 201 | 6.9 | page cache | 256 KiB | none | 0.09 | 0.01 | 29 | 0 |
| chunk-copy-mkv | 201 | 6.9 | page cache | 256 KiB | sequential | 0.08 | 0.00 | 31 | 2 |
| chunk-copy-mkv | 201 | 6.9 | page cache | 256 KiB | always | 0.08 | 0.00 | 32 | 3 |
| chunk-copy-mkv | 201 | 6.9 | page cache | 1024 KiB | none | 0.09 | 0.01 | 9 | 0 |
| chunk-copy-mkv | 201 | 6.9 | page cache | 1024 KiB | sequential | 0.08 | 0.00 | 10 | 1 |
| chunk-copy-mkv | 201 | 6.9 | page cache | 1024 KiB | always | 0.08 | 0.00 | 12 | 3 |
| chunk-copy-mkv | 201 | 6.9 | page cache | 4096 KiB | none | 0.09 | 0.02 | 5 | 0 |
| chunk-copy-mkv | 201 | 6.9 | page cache | 4096 KiB | sequential | 0.09 | 0.01 | 6 | 1 |
| chunk-copy-mkv | 201 | 6.9 | page cache | 4096 KiB | always | 0.09 | 0.01 | 8 | 3 |
| chunk-copy-mkv | 201 | 6.9 | NAS 1 Gb/s | 256 KiB | none | 0.19 | 0.11 | 29 | 0 |
| chunk-copy-mkv | 201 | 6.9 | NAS 1 Gb/s | 256 KiB | sequential | 0.16 | 0.09 | 31 | 2 |
| chunk-copy-mkv | 201 | 6.9 | NAS 1 Gb/s | 256 KiB | always | 0.16 | 0.08 | 32 | 3 |
| chunk-copy-mkv | 201 | 6.9 | NAS 1 Gb/s | 1024 KiB | none | 0.19 | 0.11 | 9 | 0 |
| chunk-copy-mkv | 201 | 6.9 | NAS 1 Gb/s | 1024 KiB | sequential | 0.17 | 0.09 | 10 | 1 |
| chunk-copy-mkv | 201 | 6.9 | NAS 1 Gb/s | 1024 KiB | always | 0.17 | 0.09 | 12 | 3 |
| chunk-copy-mkv | 201 | 6.9 | NAS 1 Gb/s | 4096 KiB | none | 0.29 | 0.21 | 5 | 0 |
| chunk-copy-mkv | 201 | 6.9 | NAS 1 Gb/s | 4096 KiB | sequential | 0.27 | 0.20 | 6 | 1 |
| chunk-copy-mkv | 201 | 6.9 | NAS 1 Gb/s | 4096 KiB | always | 0.30 | 0.23 | 8 | 3 |
| chunk-copy-mkv | 201 | 6.9 | local HDD | 256 KiB | none | 0.16 | 0.08 | 29 | 0 |
| chunk-copy-mkv | 201 | 6.9 | local HDD | 256 KiB | sequential | 0.14 | 0.06 | 31 | 2 |
| chunk-copy-mkv | 201 | 6.9 | local HDD | 256 KiB | always | 0.14 | 0.06 | 32 | 3 |
| chunk-copy-mkv | 201 | 6.9 | local HDD | 1024 KiB | none | 0.17 | 0.09 | 9 | 0 |
| chunk-copy-mkv | 201 | 6.9 | local HDD | 1024 KiB | sequential | 0.15 | 0.07 | 10 | 1 |
| chunk-copy-mkv | 201 | 6.9 | local HDD | 1024 KiB | always | 0.15 | 0.07 | 12 | 3 |
| chunk-copy-mkv | 201 | 6.9 | local HDD | 4096 KiB | none | 0.24 | 0.16 | 5 | 0 |
| chunk-copy-mkv | 201 | 6.9 | local HDD | 4096 KiB | sequential | 0.23 | 0.15 | 6 | 1 |
| chunk-copy-mkv | 201 | 6.9 | local HDD | 4096 KiB | always | 0.24 | 0.16 | 8 | 3 |
| chunk-copy-mp4 | 198 | 6.8 | page cache | 256 KiB | none | 0.11 | 0.01 | 29 | 0 |
| chunk-copy-mp4 | 198 | 6.8 | page cache | 256 KiB | sequential | 0.10 | 0.00 | 32 | 2 |
| chunk-copy-mp4 | 198 | 6.8 | page cache | 256 KiB | always | 0.10 | 0.00 | 34 | 3 |
| chunk-copy-mp4 | 198 | 6.8 | page cache | 1024 KiB | none | 0.11 | 0.01 | 9 | 0 |
| chunk-copy-mp4 | 198 | 6.8 | page cache | 1024 KiB | sequential | 0.10 | 0.00 | 10 | 1 |
| chunk-copy-mp4 | 198 | 6.8 | page cache | 1024 KiB | always | 0.10 | 0.00 | 12 | 3 |
| chunk-copy-mp4 | 198 | 6.8 | page cache | 4096 KiB | none | 0.12 | 0.02 | 5 | 0 |
| chunk-copy-mp4 | 198 | 6.8 | page cache | 4096 KiB | sequential | 0.11 | 0.01 | 6 | 1 |
| chunk-copy-mp4 | 198 | 6.8 | page cache | 4096 KiB | always | 0.11 | 0.01 | 8 | 3 |
| chunk-copy-mp4 | 198 | 6.8 | NAS 1 Gb/s | 256 KiB | none | 0.21 | 0.11 | 29 | 0 |
| chunk-copy-mp4 | 198 | 6.8 | NAS 1 Gb/s | 256 KiB | sequential | 0.18 | 0.08 | 32 | 2 |
| chunk-copy-mp4 | 198 | 6.8 | NAS 1 Gb/s | 256 KiB | always | 0.18 | 0.08 | 34 | 3 |
| chunk-copy-mp4 | 198 | 6.8 | NAS 1 Gb/s | 1024 KiB | none | 0.21 | 0.11 | 9 | 0 |
| chunk-copy-mp4 | 198 | 6.8 | NAS 1 Gb/s | 1024 KiB | sequential | 0.18 | 0.08 | 10 | 1 |
| chunk-copy-mp4 | 198 | 6.8 | NAS 1 Gb/s | 1024 KiB | always | 0.18 | 0.08 | 12 | 3 |
| chunk-copy-mp4 | 198 | 6.8 | NAS 1 Gb/s | 4096 KiB | none | 0.31 | 0.21 | 5 | 0 |
| chunk-copy-mp4 | 198 | 6.8 | NAS 1 Gb/s | 4096 KiB | sequential | 0.29 | 0.19 | 6 | 1 |
| chunk-copy-mp4 | 198 | 6.8 | NAS 1 Gb/s | 4096 KiB | always | 0.31 | 0.21 | 8 | 3 |
| chunk-copy-mp4 | 198 | 6.8 | local HDD | 256 KiB | none | 0.18 | 0.08 | 29 | 0 |
| chunk-copy-mp4 | 198 | 6.8 | local HDD | 256 KiB | sequential | 0.16 | 0.06 | 32 | 2 |
| chunk-copy-mp4 | 198 | 6.8 | local HDD | 256 KiB | always | 0.16 | 0.06 | 34 | 3 |
| chunk-copy-mp4 | 198 | 6.8 | local HDD | 1024 KiB | none | 0.19 | 0.09 | 9 | 0 |
| chunk-copy-mp4 | 198 | 6.8 | local HDD | 1024 KiB | sequential | 0.16 | 0.06 | 10 | 1 |
| chunk-copy-mp4 | 198 | 6.8 | local HDD | 1024 KiB | always | 0.16 | 0.06 | 12 | 3 |
| chunk-copy-mp4 | 198 | 6.8 | local HDD | 4096 KiB | none | 0.27 | 0.16 | 5 | 0 |
| chunk-copy-mp4 | 198 | 6.8 | local HDD | 4096 KiB | sequential | 0.24 | 0.14 | 6 | 1 |
| chunk-copy-mp4 | 198 | 6.8 | local HDD | 4096 KiB | always | 0.25 | 0.15 | 8 | 3 |
| chunk-encode-mkv | 226 | 7.7 | page cache | 256 KiB | none | 2.91 | 0.01 | 32 | 0 |
| chunk-encode-mkv | 226 | 7.7 | page cache | 256 KiB | sequential | 2.90 | 0.00 | 34 | 2 |
| chunk-encode-mkv | 226 | 7.7 | page cache | 256 KiB | always | 2.90 | 0.00 | 35 | 3 |
| chunk-encode-mkv | 226 | 7.7 | page cache | 1024 KiB | none | 2.91 | 0.01 | 10 | 0 |
| chunk-encode-mkv | 226 | 7.7 | page cache | 1024 KiB | sequential | 2.91 | 0.00 | 11 | 1 |
| chunk-encode-mkv | 226 | 7.7 | page cache | 1024 KiB | always | 2.90 | 0.00 | 13 | 3 |
| chunk-encode-mkv | 226 | 7.7 | page cache | 4096 KiB | none | 2.92 | 0.02 | 5 | 0 |
| chunk-encode-mkv | 226 | 7.7 | page cache | 4096 KiB | sequential | 2.92 | 0.01 | 6 | 1 |
| chunk-encode-mkv | 226 | 7.7 | page cache | 4096 KiB | always | 2.91 | 0.01 | 8 | 3 |
| chunk-encode-mkv | 226 | 7.7 | NAS 1 Gb/s | 256 KiB | none | 3.03 | 0.12 | 32 | 0 |
| chunk-encode-mkv | 226 | 7.7 | NAS 1 Gb/s | 256 KiB | sequential | 2.93 | 0.03 | 34 | 2 |
| chunk-encode-mkv | 226 | 7.7 | NAS 1 Gb/s | 256 KiB | always | 2.93 | 0.03 | 35 | 3 |
| chunk-encode-mkv | 226 | 7.7 | NAS 1 Gb/s | 1024 KiB | none | 3.02 | 0.12 | 10 | 0 |
| chunk-encode-mkv | 226 | 7.7 | NAS 1 Gb/s | 1024 KiB | sequential | 2.96 | 0.06 | 11 | 1 |
| chunk-encode-mkv | 226 | 7.7 | NAS 1 Gb/s | 1024 KiB | always | 2.95 | 0.04 | 13 | 3 |
| chunk-encode-mkv | 226 | 7.7 | NAS 1 Gb/s | 4096 KiB | none | 3.12 | 0.21 | 5 | 0 |
| chunk-encode-mkv | 226 | 7.7 | NAS 1 Gb/s | 4096 KiB | sequential | 3.08 | 0.17 | 6 | 1 |
| chunk-encode-mkv | 226 | 7.7 | NAS 1 Gb/s | 4096 KiB | always | 3.06 | 0.16 | 8 | 3 |
| chunk-encode-mkv | 226 | 7.7 | local HDD | 256 KiB | none | 2.99 | 0.09 | 32 | 0 |
| chunk-encode-mkv | 226 | 7.7 | local HDD | 256 KiB | sequential | 2.94 | 0.04 | 34 | 2 |
| chunk-encode-mkv | 226 | 7.7 | local HDD | 256 KiB | always | 2.94 | 0.03 | 35 | 3 |
| chunk-encode-mkv | 226 | 7.7 | local HDD | 1024 KiB | none | 3.00 | 0.10 | 10 | 0 |
| chunk-encode-mkv | 226 | 7.7 | local HDD | 1024 KiB | sequential | 2.96 | 0.06 | 11 | 1 |
| chunk-encode-mkv | 226 | 7.7 | local HDD | 1024 KiB | always | 2.95 | 0.05 | 13 | 3 |
| chunk-encode-mkv | 226 | 7.7 | local HDD | 4096 KiB | none | 3.07 | 0.16 | 5 | 0 |
| chunk-encode-mkv | 226 | 7.7 | local HDD | 4096 KiB | sequential | 3.04 | 0.14 | 6 | 1 |
| chunk-encode-mkv | 226 | 7.7 | local HDD | 4096 KiB | always | 3.02 | 0.12 | 8 | 3 |
| chunk-sound-mkv | 270 | 9.2 | page cache | 256 KiB | none | 0.49 | 0.01 | 37 | 0 |
| chunk-sound-mkv | 270 | 9.2 | page cache | 256 KiB | sequential | 0.49 | 0.00 | 39 | 2 |
| chunk-sound-mkv | 270 | 9.2 | page cache | 256 KiB | always | 0.48 | 0.00 | 40 | 3 |
| chunk-sound-mkv | 270 | 9.2 | page cache | 1024 KiB | none | 0.49 | 0.01 | 11 | 0 |
| chunk-sound-mkv | 270 | 9.2 | page cache | 1024 KiB | sequential | 0.49 | 0.00 | 12 | 1 |
| chunk-sound-mkv | 270 | 9.2 | page cache | 1024 KiB | always | 0.49 | 0.00 | 14 | 3 |
| chunk-sound-mkv | 270 | 9.2 | page cache | 4096 KiB | none | 0.50 | 0.02 | 5 | 0 |
| chunk-sound-mkv | 270 | 9.2 | page cache | 4096 KiB | sequential | 0.50 | 0.01 | 6 | 1 |
| chunk-sound-mkv | 270 | 9.2 | page cache | 4096 KiB | always | 0.49 | 0.01 | 8 | 3 |
| chunk-sound-mkv | 270 | 9.2 | NAS 1 Gb/s | 256 KiB | none | 0.62 | 0.14 | 37 | 0 |
| chunk-sound-mkv | 270 | 9.2 | NAS 1 Gb/s | 256 KiB | sequential | 0.52 | 0.04 | 39 | 2 |
| chunk-sound-mkv | 270 | 9.2 | NAS 1 Gb/s | 256 KiB | always | 0.52 | 0.03 | 40 | 3 |
| chunk-sound-mkv | 270 | 9.2 | NAS 1 Gb/s | 1024 KiB | none | 0.62 | 0.13 | 11 | 0 |
| chunk-sound-mkv | 270 | 9.2 | NAS 1 Gb/s | 1024 KiB | sequential | 0.54 | 0.06 | 12 | 1 |
| chunk-sound-mkv | 270 | 9.2 | NAS 1 Gb/s | 1024 KiB | always | 0.54 | 0.05 | 14 | 3 |
| chunk-sound-mkv | 270 | 9.2 | NAS 1 Gb/s | 4096 KiB | none | 0.70 | 0.21 | 5 | 0 |
| chunk-sound-mkv | 270 | 9.2 | NAS 1 Gb/s | 4096 KiB | sequential | 0.66 | 0.17 | 6 | 1 |
| chunk-sound-mkv | 270 | 9.2 | NAS 1 Gb/s | 4096 KiB | always | 0.65 | 0.17 | 8 | 3 |
| chunk-sound-mkv | 270 | 9.2 | local HDD | 256 KiB | none | 0.58 | 0.10 | 37 | 0 |
| chunk-sound-mkv | 270 | 9.2 | local HDD | 256 KiB | sequential | 0.52 | 0.04 | 39 | 2 |
| chunk-sound-mkv | 270 | 9.2 | local HDD | 256 KiB | always | 0.52 | 0.04 | 40 | 3 |
| chunk-sound-mkv | 270 | 9.2 | local HDD | 1024 KiB | none | 0.59 | 0.10 | 11 | 0 |
| chunk-sound-mkv | 270 | 9.2 | local HDD | 1024 KiB | sequential | 0.54 | 0.06 | 12 | 1 |
| chunk-sound-mkv | 270 | 9.2 | local HDD | 1024 KiB | always | 0.53 | 0.05 | 14 | 3 |
| chunk-sound-mkv | 270 | 9.2 | local HDD | 4096 KiB | none | 0.65 | 0.16 | 5 | 0 |
| chunk-sound-mkv | 270 | 9.2 | local HDD | 4096 KiB | sequential | 0.62 | 0.14 | 6 | 1 |
| chunk-sound-mkv | 270 | 9.2 | local HDD | 4096 KiB | always | 0.62 | 0.13 | 8 | 3 |
| prepare-remux-mkv | 9261 | 315.4 | page cache | 256 KiB | none | 2.92 | 0.33 | 1203 | 0 |
| prepare-remux-mkv | 9261 | 315.4 | page cache | 256 KiB | sequential | 2.59 | 0.00 | 1204 | 1 |
| prepare-remux-mkv | 9261 | 315.4 | page cache | 256 KiB | always | 2.59 | 0.00 | 1204 | 1 |
| prepare-remux-mkv | 9261 | 315.4 | page cache | 1024 KiB | none | 2.85 | 0.26 | 301 | 0 |
| prepare-remux-mkv | 9261 | 315.4 | page cache | 1024 KiB | sequential | 2.59 | 0.00 | 302 | 1 |
| prepare-remux-mkv | 9261 | 315.4 | page cache | 1024 KiB | always | 2.59 | 0.00 | 302 | 1 |
| prepare-remux-mkv | 9261 | 315.4 | page cache | 4096 KiB | none | 2.84 | 0.25 | 76 | 0 |
| prepare-remux-mkv | 9261 | 315.4 | page cache | 4096 KiB | sequential | 2.60 | 0.01 | 77 | 1 |
| prepare-remux-mkv | 9261 | 315.4 | page cache | 4096 KiB | always | 2.59 | 0.00 | 77 | 1 |
| prepare-remux-mkv | 9261 | 315.4 | NAS 1 Gb/s | 256 KiB | none | 6.78 | 4.19 | 1203 | 0 |
| prepare-remux-mkv | 9261 | 315.4 | NAS 1 Gb/s | 256 KiB | sequential | 4.55 | 1.96 | 1204 | 1 |
| prepare-remux-mkv | 9261 | 315.4 | NAS 1 Gb/s | 256 KiB | always | 4.54 | 1.95 | 1204 | 1 |
| prepare-remux-mkv | 9261 | 315.4 | NAS 1 Gb/s | 1024 KiB | none | 5.85 | 3.26 | 301 | 0 |
| prepare-remux-mkv | 9261 | 315.4 | NAS 1 Gb/s | 1024 KiB | sequential | 3.59 | 1.00 | 302 | 1 |
| prepare-remux-mkv | 9261 | 315.4 | NAS 1 Gb/s | 1024 KiB | always | 3.58 | 0.99 | 302 | 1 |
| prepare-remux-mkv | 9261 | 315.4 | NAS 1 Gb/s | 4096 KiB | none | 5.65 | 3.06 | 76 | 0 |
| prepare-remux-mkv | 9261 | 315.4 | NAS 1 Gb/s | 4096 KiB | sequential | 3.36 | 0.77 | 77 | 1 |
| prepare-remux-mkv | 9261 | 315.4 | NAS 1 Gb/s | 4096 KiB | always | 3.32 | 0.73 | 77 | 1 |
| prepare-remux-mkv | 9261 | 315.4 | local HDD | 256 KiB | none | 4.93 | 2.34 | 1203 | 0 |
| prepare-remux-mkv | 9261 | 315.4 | local HDD | 256 KiB | sequential | 3.19 | 0.60 | 1204 | 1 |
| prepare-remux-mkv | 9261 | 315.4 | local HDD | 256 KiB | always | 3.19 | 0.60 | 1204 | 1 |
| prepare-remux-mkv | 9261 | 315.4 | local HDD | 1024 KiB | none | 4.72 | 2.13 | 301 | 0 |
| prepare-remux-mkv | 9261 | 315.4 | local HDD | 1024 KiB | sequential | 2.89 | 0.30 | 302 | 1 |
| prepare-remux-mkv | 9261 | 315.4 | local HDD | 1024 KiB | always | 2.88 | 0.29 | 302 | 1 |
| prepare-remux-mkv | 9261 | 315.4 | local HDD | 4096 KiB | none | 4.69 | 2.10 | 76 | 0 |
| prepare-remux-mkv | 9261 | 315.4 | local HDD | 4096 KiB | sequential | 2.78 | 0.19 | 77 | 1 |
| prepare-remux-mkv | 9261 | 315.4 | local HDD | 4096 KiB | always | 2.76 | 0.17 | 77 | 1 |
| probe-mkv | 13 | 0.5 | page cache | 256 KiB | none | 0.04 | 0.00 | 2 | 0 |
| probe-mkv | 13 | 0.5 | page cache | 256 KiB | sequential | 0.04 | 0.00 | 3 | 1 |
| probe-mkv | 13 | 0.5 | page cache | 256 KiB | always | 0.04 | 0.00 | 3 | 1 |
| probe-mkv | 13 | 0.5 | page cache | 1024 KiB | none | 0.04 | 0.00 | 1 | 0 |
| probe-mkv | 13 | 0.5 | page cache | 1024 KiB | sequential | 0.04 | 0.00 | 1 | 0 |
| probe-mkv | 13 | 0.5 | page cache | 1024 KiB | always | 0.04 | 0.00 | 2 | 1 |
| probe-mkv | 13 | 0.5 | page cache | 4096 KiB | none | 0.04 | 0.00 | 1 | 0 |
| probe-mkv | 13 | 0.5 | page cache | 4096 KiB | sequential | 0.04 | 0.00 | 1 | 0 |
| probe-mkv | 13 | 0.5 | page cache | 4096 KiB | always | 0.04 | 0.00 | 2 | 1 |
| probe-mkv | 13 | 0.5 | NAS 1 Gb/s | 256 KiB | none | 0.05 | 0.01 | 2 | 0 |
| probe-mkv | 13 | 0.5 | NAS 1 Gb/s | 256 KiB | sequential | 0.05 | 0.01 | 3 | 1 |
| probe-mkv | 13 | 0.5 | NAS 1 Gb/s | 256 KiB | always | 0.05 | 0.01 | 3 | 1 |
| probe-mkv | 13 | 0.5 | NAS 1 Gb/s | 1024 KiB | none | 0.05 | 0.01 | 1 | 0 |
| probe-mkv | 13 | 0.5 | NAS 1 Gb/s | 1024 KiB | sequential | 0.05 | 0.01 | 1 | 0 |
| probe-mkv | 13 | 0.5 | NAS 1 Gb/s | 1024 KiB | always | 0.05 | 0.01 | 2 | 1 |
| probe-mkv | 13 | 0.5 | NAS 1 Gb/s | 4096 KiB | none | 0.08 | 0.04 | 1 | 0 |
| probe-mkv | 13 | 0.5 | NAS 1 Gb/s | 4096 KiB | sequential | 0.08 | 0.04 | 1 | 0 |
| probe-mkv | 13 | 0.5 | NAS 1 Gb/s | 4096 KiB | always | 0.08 | 0.04 | 2 | 1 |
| probe-mkv | 13 | 0.5 | local HDD | 256 KiB | none | 0.05 | 0.01 | 2 | 0 |
| probe-mkv | 13 | 0.5 | local HDD | 256 KiB | sequential | 0.05 | 0.01 | 3 | 1 |
| probe-mkv | 13 | 0.5 | local HDD | 256 KiB | always | 0.05 | 0.01 | 3 | 1 |
| probe-mkv | 13 | 0.5 | local HDD | 1024 KiB | none | 0.06 | 0.02 | 1 | 0 |
| probe-mkv | 13 | 0.5 | local HDD | 1024 KiB | sequential | 0.06 | 0.02 | 1 | 0 |
| probe-mkv | 13 | 0.5 | local HDD | 1024 KiB | always | 0.06 | 0.02 | 2 | 1 |
| probe-mkv | 13 | 0.5 | local HDD | 4096 KiB | none | 0.08 | 0.04 | 1 | 0 |
| probe-mkv | 13 | 0.5 | local HDD | 4096 KiB | sequential | 0.08 | 0.04 | 1 | 0 |
| probe-mkv | 13 | 0.5 | local HDD | 4096 KiB | always | 0.08 | 0.04 | 2 | 1 |
| probe-mp4-moov-end | 12 | 0.5 | page cache | 256 KiB | none | 0.01 | 0.00 | 4 | 0 |
| probe-mp4-moov-end | 12 | 0.5 | page cache | 256 KiB | sequential | 0.01 | 0.00 | 6 | 1 |
| probe-mp4-moov-end | 12 | 0.5 | page cache | 256 KiB | always | 0.01 | 0.00 | 8 | 2 |
| probe-mp4-moov-end | 12 | 0.5 | page cache | 1024 KiB | none | 0.01 | 0.00 | 2 | 0 |
| probe-mp4-moov-end | 12 | 0.5 | page cache | 1024 KiB | sequential | 0.01 | 0.00 | 2 | 0 |
| probe-mp4-moov-end | 12 | 0.5 | page cache | 1024 KiB | always | 0.01 | 0.00 | 4 | 2 |
| probe-mp4-moov-end | 12 | 0.5 | page cache | 4096 KiB | none | 0.02 | 0.01 | 2 | 0 |
| probe-mp4-moov-end | 12 | 0.5 | page cache | 4096 KiB | sequential | 0.02 | 0.01 | 2 | 0 |
| probe-mp4-moov-end | 12 | 0.5 | page cache | 4096 KiB | always | 0.02 | 0.01 | 4 | 2 |
| probe-mp4-moov-end | 12 | 0.5 | NAS 1 Gb/s | 256 KiB | none | 0.03 | 0.02 | 4 | 0 |
| probe-mp4-moov-end | 12 | 0.5 | NAS 1 Gb/s | 256 KiB | sequential | 0.04 | 0.03 | 6 | 1 |
| probe-mp4-moov-end | 12 | 0.5 | NAS 1 Gb/s | 256 KiB | always | 0.04 | 0.03 | 8 | 2 |
| probe-mp4-moov-end | 12 | 0.5 | NAS 1 Gb/s | 1024 KiB | none | 0.04 | 0.03 | 2 | 0 |
| probe-mp4-moov-end | 12 | 0.5 | NAS 1 Gb/s | 1024 KiB | sequential | 0.04 | 0.03 | 2 | 0 |
| probe-mp4-moov-end | 12 | 0.5 | NAS 1 Gb/s | 1024 KiB | always | 0.05 | 0.04 | 4 | 2 |
| probe-mp4-moov-end | 12 | 0.5 | NAS 1 Gb/s | 4096 KiB | none | 0.10 | 0.09 | 2 | 0 |
| probe-mp4-moov-end | 12 | 0.5 | NAS 1 Gb/s | 4096 KiB | sequential | 0.10 | 0.09 | 2 | 0 |
| probe-mp4-moov-end | 12 | 0.5 | NAS 1 Gb/s | 4096 KiB | always | 0.14 | 0.13 | 4 | 2 |
| probe-mp4-moov-end | 12 | 0.5 | local HDD | 256 KiB | none | 0.04 | 0.03 | 4 | 0 |
| probe-mp4-moov-end | 12 | 0.5 | local HDD | 256 KiB | sequential | 0.05 | 0.03 | 6 | 1 |
| probe-mp4-moov-end | 12 | 0.5 | local HDD | 256 KiB | always | 0.05 | 0.03 | 8 | 2 |
| probe-mp4-moov-end | 12 | 0.5 | local HDD | 1024 KiB | none | 0.04 | 0.03 | 2 | 0 |
| probe-mp4-moov-end | 12 | 0.5 | local HDD | 1024 KiB | sequential | 0.04 | 0.03 | 2 | 0 |
| probe-mp4-moov-end | 12 | 0.5 | local HDD | 1024 KiB | always | 0.05 | 0.04 | 4 | 2 |
| probe-mp4-moov-end | 12 | 0.5 | local HDD | 4096 KiB | none | 0.09 | 0.07 | 2 | 0 |
| probe-mp4-moov-end | 12 | 0.5 | local HDD | 4096 KiB | sequential | 0.09 | 0.07 | 2 | 0 |
| probe-mp4-moov-end | 12 | 0.5 | local HDD | 4096 KiB | always | 0.11 | 0.10 | 4 | 2 |
| readthrough-mkv | 9261 | 315.4 | page cache | 256 KiB | none | 11.79 | 0.33 | 1203 | 0 |
| readthrough-mkv | 9261 | 315.4 | page cache | 256 KiB | sequential | 11.46 | 0.00 | 1204 | 1 |
| readthrough-mkv | 9261 | 315.4 | page cache | 256 KiB | always | 11.46 | 0.00 | 1204 | 1 |
| readthrough-mkv | 9261 | 315.4 | page cache | 1024 KiB | none | 11.72 | 0.26 | 301 | 0 |
| readthrough-mkv | 9261 | 315.4 | page cache | 1024 KiB | sequential | 11.46 | 0.00 | 302 | 1 |
| readthrough-mkv | 9261 | 315.4 | page cache | 1024 KiB | always | 11.46 | 0.00 | 302 | 1 |
| readthrough-mkv | 9261 | 315.4 | page cache | 4096 KiB | none | 11.71 | 0.25 | 76 | 0 |
| readthrough-mkv | 9261 | 315.4 | page cache | 4096 KiB | sequential | 11.47 | 0.01 | 77 | 1 |
| readthrough-mkv | 9261 | 315.4 | page cache | 4096 KiB | always | 11.46 | 0.00 | 77 | 1 |
| readthrough-mkv | 9261 | 315.4 | NAS 1 Gb/s | 256 KiB | none | 15.65 | 4.19 | 1203 | 0 |
| readthrough-mkv | 9261 | 315.4 | NAS 1 Gb/s | 256 KiB | sequential | 11.58 | 0.12 | 1204 | 1 |
| readthrough-mkv | 9261 | 315.4 | NAS 1 Gb/s | 256 KiB | always | 11.57 | 0.11 | 1204 | 1 |
| readthrough-mkv | 9261 | 315.4 | NAS 1 Gb/s | 1024 KiB | none | 14.72 | 3.26 | 301 | 0 |
| readthrough-mkv | 9261 | 315.4 | NAS 1 Gb/s | 1024 KiB | sequential | 11.49 | 0.03 | 302 | 1 |
| readthrough-mkv | 9261 | 315.4 | NAS 1 Gb/s | 1024 KiB | always | 11.48 | 0.02 | 302 | 1 |
| readthrough-mkv | 9261 | 315.4 | NAS 1 Gb/s | 4096 KiB | none | 14.52 | 3.06 | 76 | 0 |
| readthrough-mkv | 9261 | 315.4 | NAS 1 Gb/s | 4096 KiB | sequential | 11.54 | 0.08 | 77 | 1 |
| readthrough-mkv | 9261 | 315.4 | NAS 1 Gb/s | 4096 KiB | always | 11.50 | 0.04 | 77 | 1 |
| readthrough-mkv | 9261 | 315.4 | local HDD | 256 KiB | none | 13.80 | 2.34 | 1203 | 0 |
| readthrough-mkv | 9261 | 315.4 | local HDD | 256 KiB | sequential | 11.48 | 0.02 | 1204 | 1 |
| readthrough-mkv | 9261 | 315.4 | local HDD | 256 KiB | always | 11.48 | 0.02 | 1204 | 1 |
| readthrough-mkv | 9261 | 315.4 | local HDD | 1024 KiB | none | 13.59 | 2.13 | 301 | 0 |
| readthrough-mkv | 9261 | 315.4 | local HDD | 1024 KiB | sequential | 11.48 | 0.02 | 302 | 1 |
| readthrough-mkv | 9261 | 315.4 | local HDD | 1024 KiB | always | 11.48 | 0.02 | 302 | 1 |
| readthrough-mkv | 9261 | 315.4 | local HDD | 4096 KiB | none | 13.56 | 2.10 | 76 | 0 |
| readthrough-mkv | 9261 | 315.4 | local HDD | 4096 KiB | sequential | 11.52 | 0.06 | 77 | 1 |
| readthrough-mkv | 9261 | 315.4 | local HDD | 4096 KiB | always | 11.50 | 0.04 | 77 | 1 |
| subtitles-mkv | 9261 | 315.4 | page cache | 256 KiB | none | 1.18 | 0.33 | 1203 | 0 |
| subtitles-mkv | 9261 | 315.4 | page cache | 256 KiB | sequential | 0.86 | 0.01 | 1204 | 1 |
| subtitles-mkv | 9261 | 315.4 | page cache | 256 KiB | always | 0.86 | 0.01 | 1204 | 1 |
| subtitles-mkv | 9261 | 315.4 | page cache | 1024 KiB | none | 1.11 | 0.26 | 301 | 0 |
| subtitles-mkv | 9261 | 315.4 | page cache | 1024 KiB | sequential | 0.85 | 0.00 | 302 | 1 |
| subtitles-mkv | 9261 | 315.4 | page cache | 1024 KiB | always | 0.85 | 0.00 | 302 | 1 |
| subtitles-mkv | 9261 | 315.4 | page cache | 4096 KiB | none | 1.09 | 0.25 | 76 | 0 |
| subtitles-mkv | 9261 | 315.4 | page cache | 4096 KiB | sequential | 0.85 | 0.01 | 77 | 1 |
| subtitles-mkv | 9261 | 315.4 | page cache | 4096 KiB | always | 0.85 | 0.00 | 77 | 1 |
| subtitles-mkv | 9261 | 315.4 | NAS 1 Gb/s | 256 KiB | none | 5.03 | 4.19 | 1203 | 0 |
| subtitles-mkv | 9261 | 315.4 | NAS 1 Gb/s | 256 KiB | sequential | 4.24 | 3.40 | 1204 | 1 |
| subtitles-mkv | 9261 | 315.4 | NAS 1 Gb/s | 256 KiB | always | 4.24 | 3.39 | 1204 | 1 |
| subtitles-mkv | 9261 | 315.4 | NAS 1 Gb/s | 1024 KiB | none | 4.11 | 3.26 | 301 | 0 |
| subtitles-mkv | 9261 | 315.4 | NAS 1 Gb/s | 1024 KiB | sequential | 3.31 | 2.47 | 302 | 1 |
| subtitles-mkv | 9261 | 315.4 | NAS 1 Gb/s | 1024 KiB | always | 3.30 | 2.46 | 302 | 1 |
| subtitles-mkv | 9261 | 315.4 | NAS 1 Gb/s | 4096 KiB | none | 3.90 | 3.06 | 76 | 0 |
| subtitles-mkv | 9261 | 315.4 | NAS 1 Gb/s | 4096 KiB | sequential | 3.12 | 2.27 | 77 | 1 |
| subtitles-mkv | 9261 | 315.4 | NAS 1 Gb/s | 4096 KiB | always | 3.08 | 2.23 | 77 | 1 |
| subtitles-mkv | 9261 | 315.4 | local HDD | 256 KiB | none | 3.18 | 2.34 | 1203 | 0 |
| subtitles-mkv | 9261 | 315.4 | local HDD | 256 KiB | sequential | 2.42 | 1.58 | 1204 | 1 |
| subtitles-mkv | 9261 | 315.4 | local HDD | 256 KiB | always | 2.42 | 1.58 | 1204 | 1 |
| subtitles-mkv | 9261 | 315.4 | local HDD | 1024 KiB | none | 2.97 | 2.13 | 301 | 0 |
| subtitles-mkv | 9261 | 315.4 | local HDD | 1024 KiB | sequential | 2.19 | 1.34 | 302 | 1 |
| subtitles-mkv | 9261 | 315.4 | local HDD | 1024 KiB | always | 2.18 | 1.34 | 302 | 1 |
| subtitles-mkv | 9261 | 315.4 | local HDD | 4096 KiB | none | 2.94 | 2.10 | 76 | 0 |
| subtitles-mkv | 9261 | 315.4 | local HDD | 4096 KiB | sequential | 2.16 | 1.31 | 77 | 1 |
| subtitles-mkv | 9261 | 315.4 | local HDD | 4096 KiB | always | 2.13 | 1.28 | 77 | 1 |
