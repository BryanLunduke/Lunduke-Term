| profile | build | run | idle CPU % fresh | idle CPU % after work | Private_Dirty fresh kB | Private_Dirty after kB | median tick ms | median effects-source ms | median paint ms | work CPU s | content frames/ticks | window pos | WM alive before+after |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| amber-gl-1280 | before | 1 | 12.6 | 12.4 | 85400 | 88640 | 3.53 | 11.02 | 0.52 | 10.3 | 55/684 | 10,58 | yes |
| amber-gl-1280 | before | 2 | 13.0 | 13.2 | 85208 | 89856 | 3.60 | 10.52 | 0.53 | 10.0 | 50/668 | 10,58 | yes |
| amber-gl-1280 | before | 3 | 8.7 | 11.2 | 85344 | 88576 | 3.37 | 10.69 | 0.39 | 10.1 | 61/568 | 10,58 | yes |
| amber-gl-1280 | after | 1 | 10.2 | 11.5 | 83216 | 86432 | 3.38 | 10.45 | 0.41 | 9.6 | 55/630 | 10,58 | yes |
| amber-gl-1280 | after | 2 | 13.3 | 7.9 | 83380 | 86616 | 3.72 | 11.20 | 0.69 | 10.0 | 53/267 | 10,58 | yes |
| amber-gl-1280 | after | 3 | 12.4 | 11.9 | 83204 | 86424 | 3.19 | 8.81 | 0.35 | 9.1 | 50/728 | 10,58 | yes |
| amber-cpu-1280 | before | 1 | 12.4 | 11.3 | 70532 | 74824 | 10.26 | 13.41 | 0.67 | 10.6 | 58/448 | 10,58 | yes |
| amber-cpu-1280 | before | 2 | 12.8 | 1.0 | 70528 | 74828 | 9.86 | 13.53 | 0.53 | 10.6 | 58/203 | 10,58 | yes |
| amber-cpu-1280 | before | 3 | 12.3 | 11.0 | 70528 | 74712 | 9.78 | 13.06 | 0.44 | 9.8 | 54/471 | 10,58 | yes |
| amber-cpu-1280 | after | 1 | 14.0 | 12.9 | 44528 | 48844 | 10.84 | 15.84 | 0.64 | 11.2 | 56/485 | 10,58 | yes |
| amber-cpu-1280 | after | 2 | 10.4 | 10.0 | 44528 | 48664 | 10.71 | 15.74 | 0.49 | 11.2 | 58/400 | 10,58 | yes |
| amber-cpu-1280 | after | 3 | 13.0 | 12.9 | 44516 | 48700 | 10.45 | 15.41 | 0.43 | 11.4 | 59/487 | 10,58 | yes |

| profile | idle CPU % fresh | idle CPU % after work | PD fresh kB (delta) | PD after kB (delta) | median tick ms | median effects-source ms |
|---|---|---|---|---|---|---|
| amber-gl-1280 | 12.6 -> 12.4 | 12.4 -> 11.5 | 85344 -> 83216 (-2128) | 88640 -> 86432 (-2208) | 3.53 -> 3.38 | 10.69 -> 10.45 |
| amber-cpu-1280 | 12.4 -> 13 | 11 -> 12.9 | 70528 -> 44528 (-26000) | 74824 -> 48700 (-26124) | 9.86 -> 10.71 | 13.41 -> 15.74 |
