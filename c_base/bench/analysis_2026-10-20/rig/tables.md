
### 1. Human pace, 192 two-player games

runs: BASE 3 (+0 count), NOW-epoll 3 (+0 count), NOW-uring 3 (+0 count); backends seen: BASE ['epoll-implicit'], NOW-epoll ['epoll'], NOW-uring ['io_uring']

| metric | BASE | NOW-epoll | NOW-uring |
|---|---|---|---|
| server µs/move (cgroup) | 176.0 [172.1-177.4] | 76.9 [73.8-77.8] | 78.8 [73.8-78.9] |
| server µs/move (perf task-clock) | 164.5 [159.8-164.8] | 72.1 [69.4-73.0] | 74.0 [69.3-74.5] |
| server cores used | 0.017 | 0.007 [0.007-0.008] | 0.008 [0.007-0.008] |
| syscalls/move | 21.26 [21.19-21.40] | 3.96 [3.96-3.97] | 1.98 [1.96-1.98] |
| moves/s | 97 [97-97] | 97 [97-98] | 98 [98-98] |
| moves in window | 3606 [3587-3611] | 3602 [3594-3641] | 3625 [3622-3626] |
| move p50 ms | 0.266 [0.266-0.268] | 0.145 [0.139-0.146] | 0.152 [0.149-0.157] |
| move p95 ms | 0.484 [0.472-0.523] | 0.295 [0.275-0.295] | 0.301 [0.286-0.312] |
| move p99 ms | 0.647 [0.640-0.959] | 0.405 [0.394-0.433] | 0.412 [0.411-0.453] |
| push p50 ms | 0.263 [0.263-0.266] | 0.160 [0.153-0.162] | 0.151 [0.148-0.157] |
| push p95 ms | 0.461 [0.460-0.480] | 0.296 [0.278-0.296] | 0.283 [0.265-0.285] |
| push max ms | 3.9 [1.9-10.4] | 1.7 [1.1-5.0] | 2.0 [0.5-3.1] |
| move max ms | 3.9 [2.0-10.5] | 1.9 [1.4-3.0] | 2.0 [1.9-3.1] |
| lost pushes | 0 | 0 | 0 |
| server RSS max in window (MB) | 10.6 [10.6-10.7] | 12.5 [12.3-12.5] | 12.9 [12.9-13.0] |
| server RSS high-water (MB) | 10.6 [10.6-10.7] | 12.5 [12.4-12.5] | 12.9 [12.9-13.0] |
| data-disk bytes written/move (whole run, after sync) | 11980 [11658-12733] | 1437 [1423-1442] | 1489 [1460-1553] |
| journal bytes/move (whole run) | - | 217 [217-218] | 217 |
| WAL bytes/move (frames x 4120, whole run) | - | 4684 [4666-4708] | 4731 [4705-4780] |
| refused moves in window | 30 [23-31] | 20 [19-27] | 21 [14-24] |
| games finished | 0 | 0 | 0 |
| window s | 37.1 [37.1-37.1] | 37.1 [37.1-37.1] | 37.1 [37.1-37.1] |
| setup s (before window) | 4.0 [3.9-4.1] | 3.9 [3.8-4.0] | 4.0 [4.0-4.0] |

### 2. Saturation, 192 two-player games

runs: BASE 3 (+0 count), NOW-epoll 3 (+1 count), NOW-uring 3 (+0 count); backends seen: BASE ['epoll-implicit'], NOW-epoll ['epoll'], NOW-uring ['io_uring']

| metric | BASE | NOW-epoll | NOW-uring |
|---|---|---|---|
| server µs/move (cgroup) | 58.8 [58.3-59.3] | 47.6 [45.3-50.0] | 36.1 [35.8-45.8] |
| server µs/move (perf task-clock) | 59.2 [58.6-59.6] | 45.3 [43.5-47.6] | 36.3 [35.9-44.4] |
| server cores used | 0.995 [0.994-0.996] | 0.994 [0.990-0.995] | 0.993 [0.992-0.994] |
| syscalls/move | 20.96 [20.93-20.99] | 8.64 [7.90-8.86] | 4.23 [4.16-6.05] |
| mallocs/move (count build) | - | 2.07 | - |
| moves/s | 16933 [16763-17100] | 20882 [19793-21956] | 27537 [21668-27748] |
| moves in window | 271548 [268791-274339] | 334699 [317292-352059] | 441721 [347376-444741] |
| move p50 ms | 8.640 [8.637-8.843] | 11.038 [10.168-11.346] | 7.194 [7.074-10.606] |
| move p95 ms | 48.562 [48.491-49.069] | 23.907 [23.162-24.596] | 19.671 [19.628-25.484] |
| move p99 ms | 55.375 [53.822-57.870] | 31.804 [31.276-36.682] | 31.129 [30.807-33.655] |
| push p50 ms | 8.567 [8.566-8.763] | 11.093 [10.474-11.406] | 6.349 [6.240-10.167] |
| push p95 ms | 48.367 [48.330-48.907] | 23.543 [22.887-24.310] | 18.993 [18.839-24.666] |
| push max ms | 1155.1 [1110.0-1427.2] | 1747.5 [1723.3-2042.3] | 1777.9 [1710.2-1839.4] |
| move max ms | 96.8 [92.7-104.2] | 149.5 [123.8-208.6] | 143.3 [119.6-188.7] |
| lost pushes | 0 | 0 | 0 |
| server RSS max in window (MB) | 14.1 [14.0-14.2] | 65.3 [64.5-65.8] | 69.4 [68.6-69.6] |
| server RSS high-water (MB) | 14.2 [14.1-14.3] | 65.8 [65.6-65.8] | 69.4 [69.3-69.9] |
| data-disk bytes written/move (whole run, after sync) | 11747 [11732-11760] | 1228 [1224-1228] | 1233 [1233-1234] |
| journal bytes/move (whole run) | - | 59 [58-59] | 58 |
| WAL bytes/move (frames x 4120, whole run) | - | 1024 [1012-1027] | 1020 [1015-1025] |
| refused moves in window | 26636 [26409-27023] | 25532 [25148-26218] | 28254 [26890-28984] |
| games finished | 4104 [3975-4112] | 4542 [4466-4671] | 5604 [5576-5653] |
| window s | 16.0 | 16.0 | 16.0 [16.0-16.0] |

### 3a. Human pace, 48 eight-player tables, 3 spectators each

runs: BASE 3 (+0 count), NOW-epoll 3 (+0 count), NOW-uring 3 (+0 count); backends seen: BASE ['epoll-implicit'], NOW-epoll ['epoll'], NOW-uring ['io_uring']

| metric | BASE | NOW-epoll | NOW-uring |
|---|---|---|---|
| server µs/move (cgroup) | 390.4 [384.3-401.2] | 220.9 [219.6-223.0] | 218.6 [198.6-220.7] |
| server µs/move (perf task-clock) | 378.5 [371.7-390.1] | 217.2 [214.7-218.4] | 215.4 [195.3-217.0] |
| server cores used | 0.032 [0.031-0.033] | 0.018 | 0.018 [0.016-0.018] |
| syscalls/move | 34.58 [33.87-35.03] | 12.39 [12.37-12.48] | 2.01 |
| moves/s | 82 [81-82] | 81 [81-81] | 81 [80-82] |
| moves in window | 3028 [3017-3035] | 3008 [3007-3014] | 2992 [2984-3044] |
| move p50 ms | 0.447 [0.444-0.447] | 0.194 [0.188-0.195] | 0.265 [0.262-0.270] |
| move p95 ms | 0.989 [0.943-1.028] | 0.429 [0.412-0.473] | 0.611 [0.574-0.633] |
| move p99 ms | 1.445 [1.357-1.516] | 0.743 [0.688-0.810] | 0.946 [0.900-1.022] |
| push p50 ms | 0.421 [0.418-0.422] | 0.274 [0.269-0.274] | 0.270 [0.264-0.276] |
| push p95 ms | 0.894 [0.849-0.971] | 0.596 [0.578-0.648] | 0.618 [0.548-0.649] |
| push max ms | 5.1 [4.3-12.2] | 3.1 [3.0-6.3] | 4.0 [2.8-4.2] |
| move max ms | 5.0 [4.2-12.1] | 2.8 [1.2-3.9] | 2.8 [2.4-2.8] |
| lost pushes | 0 | 0 | 0 |
| server RSS max in window (MB) | 11.9 [10.7-12.0] | 14.2 [14.0-14.2] | 14.2 [14.2-14.6] |
| server RSS high-water (MB) | 11.9 [10.8-12.0] | 14.2 [14.0-14.2] | 14.2 [14.2-14.6] |
| data-disk bytes written/move (whole run, after sync) | 10722 [10373-12677] | 1618 [1555-1736] | 1618 [1539-1792] |
| journal bytes/move (whole run) | - | 325 [324-329] | 326 [323-329] |
| WAL bytes/move (frames x 4120, whole run) | - | 7139 [7032-7184] | 7084 [6965-7257] |
| refused moves in window | 33 [28-35] | 22 [15-25] | 25 [22-29] |
| games finished | 0 | 0 | 0 |
| window s | 37.1 [37.1-37.1] | 37.1 [37.1-37.1] | 37.1 [37.1-37.1] |
| setup s (before window) | 3.9 [3.7-4.0] | 3.8 [3.8-3.9] | 3.9 [3.9-3.9] |

### 3b. Saturation, 48 eight-player tables, 3 spectators each

runs: BASE 3 (+0 count), NOW-epoll 3 (+0 count), NOW-uring 3 (+0 count); backends seen: BASE ['epoll-implicit'], NOW-epoll ['epoll'], NOW-uring ['io_uring']

| metric | BASE | NOW-epoll | NOW-uring |
|---|---|---|---|
| server µs/move (cgroup) | 94.0 [93.8-95.7] | 56.3 [55.1-57.7] | 43.9 [42.8-45.7] |
| server µs/move (perf task-clock) | 95.0 [94.8-96.7] | 56.7 [55.5-58.1] | 45.9 [44.8-47.9] |
| server cores used | 0.997 | 0.943 [0.936-0.949] | 0.933 [0.933-0.935] |
| syscalls/move | 24.86 [24.64-25.11] | 6.59 [6.58-6.61] | 2.15 [2.11-2.17] |
| moves/s | 10608 [10419-10623] | 16634 [16449-17120] | 21276 [20467-21796] |
| moves in window | 170084 [167041-170235] | 266697 [263914-274511] | 341389 [328359-350167] |
| move p50 ms | 8.190 [8.166-8.401] | 4.626 [4.469-4.633] | 5.755 [5.619-5.866] |
| move p95 ms | 41.108 [40.392-41.274] | 8.438 [7.976-8.589] | 8.773 [8.741-9.489] |
| move p99 ms | 46.448 [46.368-46.487] | 11.117 [9.963-11.601] | 11.556 [11.330-13.359] |
| push p50 ms | 8.062 [8.044-8.281] | 6.689 [6.601-6.709] | 5.559 [5.440-5.664] |
| push p95 ms | 40.782 [40.143-40.890] | 12.293 [11.858-12.471] | 8.385 [8.373-9.022] |
| push max ms | 2110.8 [1984.1-2696.4] | 2274.8 [2066.4-2373.4] | 1596.0 [1550.7-1721.0] |
| move max ms | 80.9 [54.7-87.7] | 106.9 [98.9-109.5] | 126.8 [118.9-215.0] |
| lost pushes | 0 | 0 | 0 |
| server RSS max in window (MB) | 15.4 [15.0-15.5] | 41.7 [41.7-43.9] | 49.8 [49.0-49.9] |
| server RSS high-water (MB) | 15.7 [15.4-15.8] | 48.2 [47.2-49.1] | 55.4 [53.2-56.7] |
| data-disk bytes written/move (whole run, after sync) | 10623 [10600-10628] | 462 [458-479] | 475 [471-486] |
| journal bytes/move (whole run) | - | 59 | 60 [59-60] |
| WAL bytes/move (frames x 4120, whole run) | - | 416 [412-418] | 423 [422-425] |
| refused moves in window | 32220 [31946-32784] | 40206 [40095-41361] | 48512 [47506-49605] |
| games finished | 487 [477-495] | 754 [749-779] | 1005 [975-1029] |
| window s | 16.0 | 16.0 [16.0-16.0] | 16.1 [16.0-16.1] |

### 4. 192 games, one human (2-4 s think) against one octogen bot, bot timers / 100

runs: BASE 3 (+0 count), NOW-epoll 3 (+0 count), NOW-uring 3 (+0 count); backends seen: BASE ['epoll-implicit'], NOW-epoll ['epoll'], NOW-uring ['io_uring']

| metric | BASE | NOW-epoll | NOW-uring |
|---|---|---|---|
| server µs per table version (human+bot moves, cgroup) | 9835.9 [9756.6-9844.2] | 9837.7 [9828.7-9939.1] | 9830.2 [9728.8-9989.4] |
| server µs per table version (perf) | 9829.9 [9751.4-9838.5] | 9836.2 [9827.3-9933.5] | 9825.0 [9724.3-9984.5] |
| server cores used | 0.999 | 0.998 [0.997-0.998] | 0.998 [0.997-0.998] |
| syscalls per table version | 22.64 [22.61-22.72] | 6.07 [6.04-6.13] | 5.08 [5.05-5.09] |
| human moves/s | 48 [48-49] | 48 [48-49] | 48 [48-48] |
| human moves in window | 1703 [1650-1709] | 1678 [1667-1701] | 1694 [1647-1697] |
| move p50 ms | 0.162 [0.161-0.170] | 0.092 [0.091-0.093] | 0.090 [0.088-0.090] |
| move p95 ms | 0.348 [0.320-0.354] | 0.198 [0.193-0.198] | 0.194 [0.183-0.194] |
| move p99 ms | 0.545 [0.497-0.589] | 0.279 [0.271-0.281] | 0.280 [0.239-0.290] |
| move max ms | 2.1 [1.6-3.5] | 1.2 [0.6-2.0] | 3.4 [0.7-18.1] |
| lost pushes | 0 | 0 | 0 |
| server RSS max in window (MB) | 10.6 [10.6-10.6] | 11.1 [11.1-11.2] | 11.7 [11.6-11.7] |
| server RSS high-water (MB) | 10.6 [10.6-10.7] | 11.2 [11.1-11.2] | 11.7 [11.7-11.7] |
| data-disk bytes written/move (whole run, after sync) | 26556 [26268-27718] | 2516 [2513-2544] | 2577 [2570-2614] |
| journal bytes/move (whole run) | - | 299 [299-300] | 300 [299-300] |
| WAL bytes/move (frames x 4120, whole run) | - | 5145 [5132-5166] | 5161 [5135-5203] |
| refused moves in window | 0 [0-1] | 0 | 0 |
| games finished | 0 | 0 | 0 |
| window s | 35.1 [34.1-35.1] | 35.1 [34.1-35.1] | 35.1 [34.1-35.1] |
| setup s (before window) | 6.3 [6.3-6.9] | 6.5 [6.4-6.5] | 6.2 [6.1-6.6] |
