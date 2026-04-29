# Splash Sweep Results

## RQ1: Pointer-sharing vs. copy-based BFS
Rows: 60

| graph | N | dev_frac | method | method_name | median_us | bias_flips | dir_entries |
|---|---|---|---|---|---|---|---|
| barabasi_albert | 50 | 0.0 | A | pointer-sharing | 0 | 0 | 4146 |
| barabasi_albert | 50 | 0.0 | B | copy-based | 24831 | 0 | 4096 |
| barabasi_albert | 50 | 0.25 | A | pointer-sharing | 0 | 0 | 4146 |
| barabasi_albert | 50 | 0.25 | B | copy-based | 24891 | 0 | 4096 |
| barabasi_albert | 50 | 0.5 | A | pointer-sharing | 0 | 0 | 4146 |
| barabasi_albert | 50 | 0.5 | B | copy-based | 24851 | 0 | 4096 |
| barabasi_albert | 50 | 0.75 | A | pointer-sharing | 0 | 0 | 4146 |
| barabasi_albert | 50 | 0.75 | B | copy-based | 24821 | 0 | 4096 |
| barabasi_albert | 50 | 1.0 | A | pointer-sharing | 0 | 0 | 4146 |
| barabasi_albert | 50 | 1.0 | B | copy-based | 25330 | 0 | 4096 |
| barabasi_albert | 100 | 0.0 | A | pointer-sharing | 2 | 0 | 4196 |
| barabasi_albert | 100 | 0.0 | B | copy-based | 51901 | 0 | 4096 |
| barabasi_albert | 100 | 0.25 | A | pointer-sharing | 1 | 0 | 4196 |
| barabasi_albert | 100 | 0.25 | B | copy-based | 51698 | 0 | 4096 |
| barabasi_albert | 100 | 0.5 | A | pointer-sharing | 1 | 0 | 4196 |
| barabasi_albert | 100 | 0.5 | B | copy-based | 51896 | 0 | 4096 |
| barabasi_albert | 100 | 0.75 | A | pointer-sharing | 1 | 0 | 4196 |
| barabasi_albert | 100 | 0.75 | B | copy-based | 51546 | 0 | 4096 |
| barabasi_albert | 100 | 1.0 | A | pointer-sharing | 1 | 0 | 4196 |
| barabasi_albert | 100 | 1.0 | B | copy-based | 53853 | 0 | 4096 |
| barabasi_albert | 200 | 0.0 | A | pointer-sharing | 4 | 0 | 4296 |
| barabasi_albert | 200 | 0.0 | B | copy-based | 100782 | 0 | 4096 |
| barabasi_albert | 200 | 0.25 | A | pointer-sharing | 3 | 0 | 4296 |
| barabasi_albert | 200 | 0.25 | B | copy-based | 101242 | 0 | 4096 |
| barabasi_albert | 200 | 0.5 | A | pointer-sharing | 3 | 0 | 4296 |
| barabasi_albert | 200 | 0.5 | B | copy-based | 100867 | 0 | 4096 |
| barabasi_albert | 200 | 0.75 | A | pointer-sharing | 3 | 0 | 4296 |
| barabasi_albert | 200 | 0.75 | B | copy-based | 100755 | 0 | 4096 |
| barabasi_albert | 200 | 1.0 | A | pointer-sharing | 3 | 0 | 4296 |
| barabasi_albert | 200 | 1.0 | B | copy-based | 102510 | 0 | 4096 |
| erdos_renyi | 50 | 0.0 | A | pointer-sharing | 1 | 0 | 4146 |
| erdos_renyi | 50 | 0.0 | B | copy-based | 24613 | 0 | 4096 |
| erdos_renyi | 50 | 0.25 | A | pointer-sharing | 0 | 0 | 4146 |
| erdos_renyi | 50 | 0.25 | B | copy-based | 24813 | 0 | 4096 |
| erdos_renyi | 50 | 0.5 | A | pointer-sharing | 0 | 0 | 4146 |
| erdos_renyi | 50 | 0.5 | B | copy-based | 24699 | 0 | 4096 |
| erdos_renyi | 50 | 0.75 | A | pointer-sharing | 0 | 0 | 4146 |
| erdos_renyi | 50 | 0.75 | B | copy-based | 24665 | 0 | 4096 |
| erdos_renyi | 50 | 1.0 | A | pointer-sharing | 0 | 0 | 4146 |
| erdos_renyi | 50 | 1.0 | B | copy-based | 24744 | 0 | 4096 |
| erdos_renyi | 100 | 0.0 | A | pointer-sharing | 2 | 0 | 256056 |
| erdos_renyi | 100 | 0.0 | B | copy-based | 49283 | 0 | 4096 |
| erdos_renyi | 100 | 0.25 | A | pointer-sharing | 2 | 0 | 4196 |
| erdos_renyi | 100 | 0.25 | B | copy-based | 51096 | 0 | 4096 |
| erdos_renyi | 100 | 0.5 | A | pointer-sharing | 3 | 0 | 4196 |
| erdos_renyi | 100 | 0.5 | B | copy-based | 49995 | 0 | 4096 |
| erdos_renyi | 100 | 0.75 | A | pointer-sharing | 1 | 0 | 4196 |
| erdos_renyi | 100 | 0.75 | B | copy-based | 51480 | 0 | 4096 |
| erdos_renyi | 100 | 1.0 | A | pointer-sharing | 1 | 0 | 4196 |
| erdos_renyi | 100 | 1.0 | B | copy-based | 52571 | 0 | 4096 |
| erdos_renyi | 200 | 0.0 | A | pointer-sharing | 3 | 0 | 256622 |
| erdos_renyi | 200 | 0.0 | B | copy-based | 102418 | 0 | 4096 |
| erdos_renyi | 200 | 0.25 | A | pointer-sharing | 3 | 0 | 4296 |
| erdos_renyi | 200 | 0.25 | B | copy-based | 100661 | 0 | 4096 |
| erdos_renyi | 200 | 0.5 | A | pointer-sharing | 3 | 0 | 4296 |
| erdos_renyi | 200 | 0.5 | B | copy-based | 100912 | 0 | 4096 |
| erdos_renyi | 200 | 0.75 | A | pointer-sharing | 3 | 0 | 4296 |
| erdos_renyi | 200 | 0.75 | B | copy-based | 99019 | 0 | 4096 |
| erdos_renyi | 200 | 1.0 | A | pointer-sharing | 16 | 0 | 4296 |
| erdos_renyi | 200 | 1.0 | B | copy-based | 102903 | 0 | 4096 |

## RQ1b: B+ tree lookup
| B | num_leaves | method | median_us | p25_us | p75_us |
|---|---|---|---|---|---|
| 4 | 2500 | Pointer-sharing lookup | 1897 | 1877 | 1939 |
| 4 | 2500 | Copy-based lookup | 3415773 | 1021374 | 4083255 |
| 16 | 625 | Pointer-sharing lookup | 1292 | 924 | 1702 |
| 16 | 625 | Copy-based lookup | 3369277 | 1867571 | 9367007 |
| 64 | 157 | Pointer-sharing lookup | 259 | 258 | 272 |
| 64 | 157 | Copy-based lookup | 311237 | 297703 | 3785347 |
| 4 | 2500 | Pointer-sharing lookup | 469 | 460 | 478 |
| 4 | 2500 | Copy-based lookup | 3566101 | 3021453 | 3902738 |
| 16 | 625 | Pointer-sharing lookup | 403 | 400 | 405 |
| 16 | 625 | Copy-based lookup | 3284199 | 2929007 | 4310230 |
| 64 | 157 | Pointer-sharing lookup | 189 | 189 | 197 |
| 64 | 157 | Copy-based lookup | 2620998 | 2268594 | 2774180 |
| 4 | 2500 | Pointer-sharing lookup | 383 | 381 | 399 |
| 4 | 2500 | Copy-based lookup | 556550 | 418791 | 674553 |
| 16 | 625 | Pointer-sharing lookup | 334 | 330 | 349 |
| 16 | 625 | Copy-based lookup | 602625 | 466462 | 930708 |
| 64 | 157 | Pointer-sharing lookup | 170 | 169 | 171 |
| 64 | 157 | Copy-based lookup | 686003 | 610044 | 905551 |

## RQ1c: Hash table PUT/GET
_No data._

## RQ2: Directory capacity sweep
| section | N | hit_rate | evictions | time_ns | norm_tput | dir_entries | log |
|---|---|---|---|---|---|---|---|
| bfs | 100 | 0.0 | 0 | 2097 | 1.0 | 13 | rq2.stdout.log |
| bfs | 500 | 0.0 | 0 | 8956 | 1.1707 | 13 | rq2.stdout.log |
| bfs | 1000 | 0.0 | 0 | 19482 | 1.0764 | 13 | rq2.stdout.log |
| bfs | 2000 | 0.0 | 0 | 41781 | 1.0038 | 13 | rq2.stdout.log |
| bfs | 5000 | 0.0 | 0 | 94301 | 1.1119 | 13 | rq2.stdout.log |
| bfs | 10000 | 0.0 | 0 | 197509 | 1.0617 | 13 | rq2.stdout.log |
| bfs | 20000 | 0.0 | 0 | 556535 | 0.7536 | 13 | rq2.stdout.log |
| bfs | 50000 | 0.0 | 0 | 3698553 | 0.2835 | 13 | rq2.stdout.log |

## RQ3: Bias mode × KV workload
| source | accel_count | experiment | theta | label | p25 | median | p75 | unit | snoop_hits | snoop_misses | bias_flips | writebacks | dev_bias_hits | host_bias_hits | back_inv | dir_entries | log |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| multigpu_n1 | 1 | 1:Shared Counter (extreme contention) |  | 1T Device-Bias ops/sec | 151663367.0 | 153154367.0 | 153484874.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | rq3.stdout.log |
| multigpu_n1 | 1 | 1:Shared Counter (extreme contention) |  | 1T Host-Bias ops/sec | 154412252.0 | 192110040.0 | 202762846.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | rq3.stdout.log |
| multigpu_n1 | 1 | 1:Shared Counter (extreme contention) |  | 2T Device-Bias ops/sec | 36190939.0 | 52559959.0 | 55880937.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | rq3.stdout.log |
| multigpu_n1 | 1 | 1:Shared Counter (extreme contention) |  | 2T Host-Bias ops/sec | 35068985.0 | 49141775.0 | 52476400.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | rq3.stdout.log |
| multigpu_n1 | 1 | 2:KV Store with Zipfian Distribution | 0.0 | Device-Bias ops/sec | 15448016.0 | 15851520.0 | 16083955.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n1 | 1 | 2:KV Store with Zipfian Distribution | 0.0 | Host-Bias ops/sec | 15708905.0 | 16098805.0 | 16572015.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n1 | 1 | 2:KV Store with Zipfian Distribution | 0.0 | Hybrid ops/sec | 14797277.0 | 15777277.0 | 16051281.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n1 | 1 | 2:KV Store with Zipfian Distribution | 0.25 | Device-Bias ops/sec | 14624238.0 | 15388595.0 | 15597222.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n1 | 1 | 2:KV Store with Zipfian Distribution | 0.25 | Host-Bias ops/sec | 14041926.0 | 15056485.0 | 15731759.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n1 | 1 | 2:KV Store with Zipfian Distribution | 0.25 | Hybrid ops/sec | 14510132.0 | 15575838.0 | 15594099.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n1 | 1 | 2:KV Store with Zipfian Distribution | 0.5 | Device-Bias ops/sec | 13754420.0 | 15873650.0 | 16495634.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n1 | 1 | 2:KV Store with Zipfian Distribution | 0.5 | Host-Bias ops/sec | 15395785.0 | 16223921.0 | 16660782.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n1 | 1 | 2:KV Store with Zipfian Distribution | 0.5 | Hybrid ops/sec | 14834290.0 | 15292046.0 | 15792791.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n1 | 1 | 2:KV Store with Zipfian Distribution | 0.75 | Device-Bias ops/sec | 15487979.0 | 15637634.0 | 15974402.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n1 | 1 | 2:KV Store with Zipfian Distribution | 0.75 | Host-Bias ops/sec | 15380879.0 | 15852676.0 | 15973869.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n1 | 1 | 2:KV Store with Zipfian Distribution | 0.75 | Hybrid ops/sec | 16121569.0 | 16559170.0 | 17258456.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n1 | 1 | 2:KV Store with Zipfian Distribution | 1.0 | Device-Bias ops/sec | 14906150.0 | 15925983.0 | 16868515.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n1 | 1 | 2:KV Store with Zipfian Distribution | 1.0 | Host-Bias ops/sec | 15519349.0 | 16855549.0 | 17066922.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n1 | 1 | 2:KV Store with Zipfian Distribution | 1.0 | Hybrid ops/sec | 15711007.0 | 16497103.0 | 16980376.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n1 | 1 | 2:KV Store with Zipfian Distribution | 1.25 | Device-Bias ops/sec | 16364801.0 | 16617481.0 | 16869161.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n1 | 1 | 2:KV Store with Zipfian Distribution | 1.25 | Host-Bias ops/sec | 16435668.0 | 16592385.0 | 16797541.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n1 | 1 | 2:KV Store with Zipfian Distribution | 1.25 | Hybrid ops/sec | 16148066.0 | 16595342.0 | 16727501.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n1 | 1 | 2:KV Store with Zipfian Distribution | 1.5 | Device-Bias ops/sec | 15919500.0 | 16189251.0 | 17002317.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n1 | 1 | 2:KV Store with Zipfian Distribution | 1.5 | Host-Bias ops/sec | 15726091.0 | 16354363.0 | 16401089.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n1 | 1 | 2:KV Store with Zipfian Distribution | 1.5 | Hybrid ops/sec | 15846873.0 | 16500735.0 | 16780705.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n1 | 1 | 3:Producer-Consumer Ring Buffer |  | Device-Bias | 0.413 | 0.448 | 0.451 | GB/s |  |  |  |  |  |  |  |  | rq3.stdout.log |
| multigpu_n1 | 1 | 3:Producer-Consumer Ring Buffer |  | latency (ns) | 14175107.0 | 14585839.0 | 15475595.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 11001 | rq3.stdout.log |
| multigpu_n1 | 1 | 3:Producer-Consumer Ring Buffer |  | Host-Bias | 0.418 | 0.442 | 0.446 | GB/s |  |  |  |  |  |  |  |  | rq3.stdout.log |
| multigpu_n1 | 1 | 3:Producer-Consumer Ring Buffer |  | latency (ns) | 14341883.0 | 14779616.0 | 15308251.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 11001 | rq3.stdout.log |
| multigpu_n2 | 2 | 1:Shared Counter (extreme contention) |  | 1T Device-Bias ops/sec | 205854502.0 | 207541217.0 | 208216211.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | rq3.stdout.log |
| multigpu_n2 | 2 | 1:Shared Counter (extreme contention) |  | 1T Host-Bias ops/sec | 207463284.0 | 208247864.0 | 208591465.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | rq3.stdout.log |
| multigpu_n2 | 2 | 1:Shared Counter (extreme contention) |  | 2T Device-Bias ops/sec | 41478320.0 | 49900921.0 | 60115391.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | rq3.stdout.log |
| multigpu_n2 | 2 | 1:Shared Counter (extreme contention) |  | 2T Host-Bias ops/sec | 35751633.0 | 42457842.0 | 50144315.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | rq3.stdout.log |
| multigpu_n2 | 2 | 2:KV Store with Zipfian Distribution | 0.0 | Device-Bias ops/sec | 15307226.0 | 16699606.0 | 17588921.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n2 | 2 | 2:KV Store with Zipfian Distribution | 0.0 | Host-Bias ops/sec | 15972017.0 | 16741753.0 | 16936673.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n2 | 2 | 2:KV Store with Zipfian Distribution | 0.0 | Hybrid ops/sec | 15989291.0 | 16659297.0 | 16835388.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n2 | 2 | 2:KV Store with Zipfian Distribution | 0.25 | Device-Bias ops/sec | 15645282.0 | 16620312.0 | 16734023.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n2 | 2 | 2:KV Store with Zipfian Distribution | 0.25 | Host-Bias ops/sec | 16406597.0 | 16835989.0 | 17091025.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n2 | 2 | 2:KV Store with Zipfian Distribution | 0.25 | Hybrid ops/sec | 13885614.0 | 16142594.0 | 16700365.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n2 | 2 | 2:KV Store with Zipfian Distribution | 0.5 | Device-Bias ops/sec | 16970296.0 | 17200889.0 | 17419451.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n2 | 2 | 2:KV Store with Zipfian Distribution | 0.5 | Host-Bias ops/sec | 16535638.0 | 16813074.0 | 17091361.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n2 | 2 | 2:KV Store with Zipfian Distribution | 0.5 | Hybrid ops/sec | 16698270.0 | 16973375.0 | 17268741.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n2 | 2 | 2:KV Store with Zipfian Distribution | 0.75 | Device-Bias ops/sec | 17178455.0 | 17561920.0 | 17897325.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n2 | 2 | 2:KV Store with Zipfian Distribution | 0.75 | Host-Bias ops/sec | 17056905.0 | 17519722.0 | 17736446.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n2 | 2 | 2:KV Store with Zipfian Distribution | 0.75 | Hybrid ops/sec | 17475866.0 | 17758749.0 | 17999872.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n2 | 2 | 2:KV Store with Zipfian Distribution | 1.0 | Device-Bias ops/sec | 17936795.0 | 18240329.0 | 18503047.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n2 | 2 | 2:KV Store with Zipfian Distribution | 1.0 | Host-Bias ops/sec | 18220853.0 | 18486981.0 | 18661604.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n2 | 2 | 2:KV Store with Zipfian Distribution | 1.0 | Hybrid ops/sec | 15469002.0 | 18488963.0 | 18979808.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n2 | 2 | 2:KV Store with Zipfian Distribution | 1.25 | Device-Bias ops/sec | 18047072.0 | 18093456.0 | 19329431.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n2 | 2 | 2:KV Store with Zipfian Distribution | 1.25 | Host-Bias ops/sec | 17951906.0 | 18326628.0 | 19099682.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n2 | 2 | 2:KV Store with Zipfian Distribution | 1.25 | Hybrid ops/sec | 14923546.0 | 17749199.0 | 18673191.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n2 | 2 | 2:KV Store with Zipfian Distribution | 1.5 | Device-Bias ops/sec | 18337029.0 | 18805978.0 | 20157376.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n2 | 2 | 2:KV Store with Zipfian Distribution | 1.5 | Host-Bias ops/sec | 17326404.0 | 18144407.0 | 18508188.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n2 | 2 | 2:KV Store with Zipfian Distribution | 1.5 | Hybrid ops/sec | 17822897.0 | 18233987.0 | 19173422.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n2 | 2 | 3:Producer-Consumer Ring Buffer |  | Device-Bias | 0.428 | 0.445 | 0.448 | GB/s |  |  |  |  |  |  |  |  | rq3.stdout.log |
| multigpu_n2 | 2 | 3:Producer-Consumer Ring Buffer |  | latency (ns) | 14255668.0 | 14462381.0 | 14931000.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 11001 | rq3.stdout.log |
| multigpu_n2 | 2 | 3:Producer-Consumer Ring Buffer |  | Host-Bias | 0.41 | 0.437 | 0.453 | GB/s |  |  |  |  |  |  |  |  | rq3.stdout.log |
| multigpu_n2 | 2 | 3:Producer-Consumer Ring Buffer |  | latency (ns) | 14119974.0 | 15046855.0 | 15597113.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 11001 | rq3.stdout.log |
| multigpu_n4 | 4 | 1:Shared Counter (extreme contention) |  | 1T Device-Bias ops/sec | 208418871.0 | 208542745.0 | 209156888.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | rq3.stdout.log |
| multigpu_n4 | 4 | 1:Shared Counter (extreme contention) |  | 1T Host-Bias ops/sec | 206331488.0 | 211081348.0 | 211103628.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | rq3.stdout.log |
| multigpu_n4 | 4 | 1:Shared Counter (extreme contention) |  | 2T Device-Bias ops/sec | 27016348.0 | 42853108.0 | 44075891.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | rq3.stdout.log |
| multigpu_n4 | 4 | 1:Shared Counter (extreme contention) |  | 2T Host-Bias ops/sec | 32802076.0 | 37155480.0 | 51675246.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | rq3.stdout.log |
| multigpu_n4 | 4 | 2:KV Store with Zipfian Distribution | 0.0 | Device-Bias ops/sec | 19002458.0 | 20377027.0 | 20720072.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n4 | 4 | 2:KV Store with Zipfian Distribution | 0.0 | Host-Bias ops/sec | 19186334.0 | 20230485.0 | 20721540.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n4 | 4 | 2:KV Store with Zipfian Distribution | 0.0 | Hybrid ops/sec | 19944375.0 | 20388677.0 | 20562567.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n4 | 4 | 2:KV Store with Zipfian Distribution | 0.25 | Device-Bias ops/sec | 20146164.0 | 20509400.0 | 20801801.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n4 | 4 | 2:KV Store with Zipfian Distribution | 0.25 | Host-Bias ops/sec | 19317922.0 | 20156539.0 | 20531511.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n4 | 4 | 2:KV Store with Zipfian Distribution | 0.25 | Hybrid ops/sec | 19022719.0 | 20252988.0 | 20855400.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n4 | 4 | 2:KV Store with Zipfian Distribution | 0.5 | Device-Bias ops/sec | 20214205.0 | 20868827.0 | 21137095.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n4 | 4 | 2:KV Store with Zipfian Distribution | 0.5 | Host-Bias ops/sec | 20446377.0 | 20973176.0 | 21314599.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n4 | 4 | 2:KV Store with Zipfian Distribution | 0.5 | Hybrid ops/sec | 20618947.0 | 21216197.0 | 21463750.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n4 | 4 | 2:KV Store with Zipfian Distribution | 0.75 | Device-Bias ops/sec | 20419606.0 | 21282382.0 | 21672590.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n4 | 4 | 2:KV Store with Zipfian Distribution | 0.75 | Host-Bias ops/sec | 20880353.0 | 22180222.0 | 22321956.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n4 | 4 | 2:KV Store with Zipfian Distribution | 0.75 | Hybrid ops/sec | 20711999.0 | 21525378.0 | 21682929.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n4 | 4 | 2:KV Store with Zipfian Distribution | 1.0 | Device-Bias ops/sec | 21575852.0 | 22449400.0 | 23510249.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n4 | 4 | 2:KV Store with Zipfian Distribution | 1.0 | Host-Bias ops/sec | 21336392.0 | 22210618.0 | 22459907.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n4 | 4 | 2:KV Store with Zipfian Distribution | 1.0 | Hybrid ops/sec | 21346065.0 | 22211827.0 | 23253845.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n4 | 4 | 2:KV Store with Zipfian Distribution | 1.25 | Device-Bias ops/sec | 22249911.0 | 22587099.0 | 23394844.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n4 | 4 | 2:KV Store with Zipfian Distribution | 1.25 | Host-Bias ops/sec | 20210913.0 | 21135688.0 | 22519043.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |
| multigpu_n4 | 4 | 2:KV Store with Zipfian Distribution | 1.25 | Hybrid ops/sec | 23066747.0 | 23444322.0 | 23590931.0 |  | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 10001 | rq3.stdout.log |

## RQ4: Allocation policy
| section | policy | time_ms | cross_dom | coh_reqs | evictions | throughput | log |
|---|---|---|---|---|---|---|---|
| bfs | Random | 1.29 | 59735 | 67 | 0 | 15460928 | rq4_alloc_N2000.stdout.log |
| bfs | Static Affinity (BFS proximity) | 1.25 | 59540 | 60 | 0 | 15982026 | rq4_alloc_N2000.stdout.log |
| bfs | Topology-Aware (BFS bisection) | 1.25 | 59910 | 53 | 0 | 16009241 | rq4_alloc_N2000.stdout.log |
| bfs | Online Migration | 1.23 | 59740 | 62 | 0 | 16194798 | rq4_alloc_N2000.stdout.log |
| hash | Random | 4.11 | 167684 | 205 |  |  | rq4_alloc_N2000.stdout.log |
| hash | Round-robin (alternate) | 3.72 | 168731 | 167 |  |  | rq4_alloc_N2000.stdout.log |
| hash | Affinity (chain near bucket) | 3.11 | 16611 | 135 |  |  | rq4_alloc_N2000.stdout.log |
| bfs | Random | 0.27 | 14817 | 37 | 0 | 18319667 | rq4_alloc_N500.stdout.log |
| bfs | Static Affinity (BFS proximity) | 0.26 | 14890 | 20 | 0 | 18759257 | rq4_alloc_N500.stdout.log |
| bfs | Topology-Aware (BFS bisection) | 0.28 | 15050 | 76 | 0 | 18018062 | rq4_alloc_N500.stdout.log |
| bfs | Online Migration | 0.27 | 14880 | 25 | 0 | 18603661 | rq4_alloc_N500.stdout.log |
| hash | Random | 4.66 | 167684 | 267 |  |  | rq4_alloc_N500.stdout.log |
| hash | Round-robin (alternate) | 4.51 | 168731 | 259 |  |  | rq4_alloc_N500.stdout.log |
| hash | Affinity (chain near bucket) | 4.36 | 16611 | 234 |  |  | rq4_alloc_N500.stdout.log |

## RQ4: Hash table device fraction
| device_fraction | median_ops_sec | p25_ops_sec | p75_ops_sec | log |
|---|---|---|---|---|
| 0.0 | 84008199 | 57100439 | 85513207 | rq4_devfrac.stdout.log |
| 0.25 | 73256316 | 67617824 | 73811632 | rq4_devfrac.stdout.log |
| 0.5 | 62715584 | 54852528 | 63632256 | rq4_devfrac.stdout.log |
| 0.75 | 67977268 | 67622396 | 68087887 | rq4_devfrac.stdout.log |
| 1.0 | 80101889 | 79673656 | 83418141 | rq4_devfrac.stdout.log |
