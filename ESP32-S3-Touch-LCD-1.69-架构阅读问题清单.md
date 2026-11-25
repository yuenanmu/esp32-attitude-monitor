# ESP32 姿态监测｜数据通信与处理阅读练习

> [!info] 阅读目标
> 从 QMI8658 的六轴数据出发，追到 MCU 校正、融合、缓存和协议输出，再继续追 PC 解析与 Web 消费。另走一次反向链路，说明命令如何被解析、执行和确认。
> 板级阅读集中在 IMU、I²C 和 USB 通信。Web 端关注消息与命令的流转。

## 阅读顺序

1. [[#S02｜硬件驱动 → MCU 采样|S02 硬件驱动]] → [[#S03｜MCU 调度、时间戳与采样率|S03 采样调度]] → [[#S04｜陀螺仪零偏校正与姿态融合|S04 校正和融合]]
2. [[#S05｜历史缓存、序号、批次与丢样|S05 缓存和序号]] → [[#S06｜MCU ↔ PC 串口协议与命令解析|S06 串口协议]] → [[#S07｜控制命令：接收、入队、执行、确认|S07 命令执行]]
3. [[#S08｜PC 端：串口字节 → 结构化数据|S08 PC 解析]] → [[#S09｜PC ↔ Web：数据订阅与命令回传|S09 Web 接口]] → [[#S10｜端到端联调与故障定位|S10 联调]]
4. [[#S01｜工程身份与构建边界|S01 工程与构建（原作答和批阅）]] 可随时回看。

## 已核对的数据路径

```text
QMI8658 → I²C → Imu::read() → imu_task
        → 陀螺仪零偏校正 → 姿态估计 → history 缓存
        → comm 批量返回 DATA / ATT → USB Serial/JTAG → PC [实现待定位]
        → Web [接口与实现待确认]

反向：Web [待确认] → PC [待确认] → comm 逐行接收并解析
      → 控制队列 → imu_task 执行 → GET_STATUS 查询结果
```

> [!note] 证据范围
> 固件路径已对照本地源码。USB Serial/JTAG 来自当前工程的控制台配置，实机串口交互待验证。检查时仓库中的 `host/`、`web/` 和 `tests/` 为空。现有 `firmware/samples.jsonl` 和 `firmware/attitude_bridge.jsonl` 只提供记录内容，PC 程序和 Web 传输方式待定位。

## 如何作答与请我批阅

简答写自己的推理和文件或运行证据。单选只勾一项，判断勾「对」或「错」，再写理由。缺源码的地方可以写「待确认」，补上下一步要找到的证据。

以后批阅统一分点。按 **判定**、**答对的地方或理解偏差**、**参考回答**、**源码证据**组织，并加粗关键概念、条件和结论。

> [!example]- 批阅请求示例
> 请批阅 S04，按题号核对我的答案和源码证据，指出理解偏差，给出参考回答和一道追问题，保留我的原答案。

## S02｜硬件驱动 → MCU 采样

**源码入口** · `firmware/main/main.cpp` 的 `app_main()` → `components/board/board_i2c.cpp` 的 `board_i2c_init()` → `components/imu/imu.cpp` 的 `Imu::init()`、`configure()`、`read()`。后续 `components/…` 路径均相对于 `firmware/`。

- [x] **S02-A｜简答** 写出从 I²C 总线创建到 `Imu::read()` 返回的调用链。样本有哪六个字段，单位在哪里设定？I²C 读取失败和本次无新数据分别怎么返回？
    - 我的答案：六轴数据，ax,ay,az,gx,gy,gz,底层是qmi8658_read_sensor_data(&imu_dev_, &raw_data);然后用ImuData接收。i2c读取失败？我只看到这个board_i2c_init函数会判断句柄的有效性。
    - 源码或运行证据：在struct ImuData {}
    - 批阅／二次作答：
        - **判定：部分正确。** 六轴字段和读取函数都找对了。
        - **调用链** · `app_main()` → `board_i2c_init()` → `i2c_new_master_bus()`；总线创建后，`imu.init()` 调用 `qmi8658_init()` 和 `configure()`。运行时由采样任务调用 `Imu::read()`。
        - **六轴单位** · `Imu::configure()` 把 `ax/ay/az` 对应的加速度设为 **m/s²**，把 `gx/gy/gz` 对应的角速度设为 **rad/s**。`ImuData` 的字段声明是字段名的证据，单位看配置函数。
        - **三种返回结果** · I²C 查询或读取失败：**错误码 + `ready=false`**。器件无新样本：**`ESP_OK` + `ready=false`**。六轴读取并复制成功：**`ESP_OK` + `ready=true`**。
        - **易混点** · `board_i2c_init()` 的空指针检查在初始化时执行。采样时的读错误从 `Imu::read()` 往上返回。
        - **源码** · `main/main.cpp:12–32`、`components/board/board_i2c.cpp:4–18`、`components/imu/imu.cpp:6–35,64–100`。

- [x] **S02-B｜单选** 一次读取返回成功，但 `ready` 为 false。判断采样任务后续行为时，先核对什么？
    - [x] A. 调用者对返回码和 `ready` 的分支
    - [ ] B. PC 上的 COM 口名称
    - [ ] C. 串口协议中的 `END`
    - 我的理由／证据：imu_task.cpp中，这个imu_task函数的几个分支，判断了ret和ready的数据。
    - 批阅／二次作答：
        - **判定：正确，选 A。** 你找对了 `imu_task()` 中处理 `ret` 和 `ready` 的分支。
        - **有无新样本** · `ESP_OK` 且 `ready=false` 时，增加 **`stats.not_ready`**，执行 `continue`，本轮跳过写入历史。
        - **读取出错** · `ret != ESP_OK` 时，增加 **`stats.read_errors`**，也会跳过本轮。
        - **源码** · `components/acquisition/imu_task.cpp:277–289`。

- [x] **S02-C｜判断** 只检查 `Imu::read()` 的返回码，就足以证明这次拿到了新样本。
    - [ ] 对
    - [x] 错
    - 我的理由／证据：还需要判断ready这个标志位，因为实际上要把数据从底层的qmi8658_read_sensor_data函数，数据赋值到这个传入的data里面
    - 批阅／二次作答：
        - **判定：正确，应判错。** 你指出了需要再检查 `ready`。
        - **读到新样本的条件** · **`ret == ESP_OK && ready`**。查询就绪状态成功时也可能返回 `ESP_OK`，如果器件还没有新数据，`data` 不更新。
        - **写入历史的条件** · 读到新样本后，还要通过**校准、融合和历史写入**这几步。
        - **源码** · `components/imu/imu.cpp:64–100`、`components/acquisition/imu_task.cpp:277–334`。

**验收** · 能用代码说明物理量从哪来、什么条件下成为一条新样本。

## S03｜MCU 调度、时间戳与采样率

**源码入口** · `components/acquisition/imu_task.cpp` 的 `timer_callback()`、`imu_task()`、`imu_sampling_start()`；`components/acquisition/include/imu_task.hpp`。

- [x] **S03-A｜简答** 从定时器到成功样本追一轮：回调执行在哪个上下文，如何唤醒任务，时间戳在哪取得？若把采样率设为 200 Hz，周期怎样计算？说明目标周期和测得的样本间隔各读哪里。
    - 我的答案：回调在imu_task.cpp中，imu_sampling_start绑定imu_task任务和imu_task_handle句柄，定时器定时回调句柄对应的任务资源。ulTaskNotifyTake(pdTRUE, portMAX_DELAY);来唤醒这个任务。但是，为什么要这一句呢？时间戳通过esp_timer_get_time();获得，并且赋值给record对象，周期计算：double measured_hz =
            stats.interval_count * 1'000'000.0 /
            static_cast<double>(stats.last_us - stats.first_us);
            目标周期： 样本间隔：state变量
    - 源码或运行证据：
    - 批阅／二次作答：
        - **判定：部分正确。** 任务句柄、时间戳 API 和实测频率公式都找对了。
        - **回调在哪执行** · `ESP_TIMER_TASK` 把回调交给 **ESP Timer 服务任务**执行，采样在另外的 `imu_task()` 里处理。
        - **谁发、谁等** · 回调用 **`xTaskNotifyGive()` 发通知**；采样任务用 **`ulTaskNotifyTake(pdTRUE, portMAX_DELAY)` 等通知**。没通知时任务阻塞，避免空转。有通知时返回清零前的计数，并由 `pdTRUE` 将计数清零。
        - **目标周期** · 默认 **10,000 µs**。切到 **200 Hz** 时，`1,000,000 ÷ 200 = 5,000 µs = 5 ms`，传给 `esp_timer_restart()`。
        - **时间戳和实测间隔** · 读成功并做零偏校正后、融合前调用 `esp_timer_get_time()`，融合有输出后用这个值填写 `record.t_us`。相邻记录的 `t_us` 相减得到**实测间隔**。`measured_hz` 用间隔数和总时长算的是**平均频率**。最小、最大和平均间隔可看 `Sampling stats` 日志。
        - **源码** · `components/acquisition/imu_task.cpp:40–72,180–218,237–269,311–329,355–385,419–441`。[FreeRTOS 任务通知](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/freertos_idf.html) · [ESP Timer 回调](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/esp_timer.html#callback-dispatch-methods)

- [x] **S03-B｜单选** 要判断改变采样率后是否真的按期获得有效样本，优先比较哪两项？
    - [ ] A. 网页刷新率与串口波特率
    - [x] B. 定时器的配置周期与成功样本时间戳差
    - [ ] C. 命令字符串长度与缓存容量
    - 我的理由／证据：网页的刷新率不是和底层同步的。要查看底层回馈回来的信息。
    - 批阅／二次作答：
        - **判定：正确，选 B。** 你抓住了应该看采样任务的实际记录。
        - **怎么比** · 在 200 Hz 配置下，用 **5,000 µs 的目标周期**对照相邻记录的 **`t_us` 差**，再看 `min_dt_us`、`max_dt_us` 和 `avg_dt_us` 判断波动。
        - **统计口径** · `update_stats()` 先执行，`history_push()` 后执行。要核对**历史缓存里实际存下来的记录频率**，应从实际入库记录计算。
        - **源码** · `components/acquisition/imu_task.cpp:53–72,319–334,355–385`。

- [x] **S03-C｜判断** 定时器上的目标频率与历史缓存中的有效样本频率在所有情况下都相等。
    - [x] 对
    - [ ] 错
    - 我的理由／证据：定时器唤醒这个imu_task函数执行，这个history_push在imu_task函数内部，同步执行。
    - 批阅／二次作答：
        - **判定：需修正，应判错。** 你观察得对，读取 IMU 和写入历史在同一个任务里。
        - **理解偏差** · **同一个任务中的顺序执行，只说明走到写入分支时的先后关系。** 有些轮次会提前 `continue`，还有些轮次会写入失败。
        - **用时间线复盘** · 假设目标周期 **5 ms**，第 5 ms 和第 15 ms 各成功入库一条，第 10 ms 那轮由于 **`ready=false`** 被跳过。两条有效历史记录就隔了 **10 ms**。
        - **其他影响因素** · 读取失败、校准期间、融合未输出、通知积压被合并，或历史写入失败。
        - **源码** · `components/acquisition/imu_task.cpp:210–218,277–334`。

**验收** · 能区分定时唤醒、器件新数据、采样成功和上报四个事件。

### S02–S03 追问题

- [ ] **S03-F1｜通知积压与历史数据** 假设 `SET_RATE 200` 已生效。采样任务来得及再次调用 `ulTaskNotifyTake(pdTRUE, portMAX_DELAY)` 前，定时器已经发了 3 次通知，没有控制命令待处理。该调用返回几？返回后还剩几次待处理通知？如果紧接着 `Imu::read()` 返回 `ESP_OK` 但 `ready == false`，这轮尝试读取几次、写几条历史记录、哪些统计会变化？请用 `imu_task.cpp` 的分支解释。
    - 我的答案：返回2，待处理的为1。读取1次，写入0条。
    - 源码或运行证据： stats.extra_notifications += notifications - 1; // 统计额外通知次数。 这个看待处理任务。
    - 批阅／二次作答：
        - **判定 · 部分正确。** 你答对了 **尝试读 1 次、写入 0 条**。
        - **通知计数 · 返回 3，清零后剩 0。** `pdTRUE` 让 `ulTaskNotifyTake()` 返回清零前的累计值，再将计数清零。这里假设清零后没有新通知。
        - **统计变化 · `backlog_events +1`、`extra_notifications +2`、`not_ready +1`。** `notifications - 1` 计算本轮合并掉的额外通知。`ESP_OK` 不增加 `read_errors`，`ready=false` 使本轮跳过历史写入。
        - **源码** · `components/acquisition/imu_task.cpp:210–218,273–289`。

## S04｜陀螺仪零偏校正与姿态融合

**源码入口** · `components/acquisition/include/gyro_calibration.hpp` 的 `add()`、`correct()`，`components/acquisition/imu_task.cpp` 校正与写缓存的位置，`components/attitude/attitude.cpp` 的 `AttitudeEstimator::update()`。

- [x] **S04-A｜简答** 追一组 `gx/gy/gz`：何时累计、何时计算零偏、校正后如何进入融合？指出校准期间是否写入历史、加速度是否被同一算法修改，以及静止要求由代码验证还是由使用者保证。
    - 我的答案：一开始会校正，需要停放2s。校正后的数据被赋值给corrected，然后这个被赋值给record.imu记录。这个record调用history_push进入历史缓存。
      矫正期间不写入，校正后的数据写入。加速度不会被修改。静止由使用者保证。
    - 源码或运行证据：
    - 批阅／二次作答：
        - **判定 · 部分正确。** 对校准期间不写历史、加速度保持原值、由使用者保持静止的理解正确。`corrected` 也确实用于 `record.imu`。
        - **校准按 200 个有效样本计数。** 校准未完成时，任务把每次成功读取且 `ready=true` 的样本交给 `calibration.add()`，累计三轴角速度。到第 200 个时分别取平均作为零偏。**100 Hz 且轮轮有新样本时约 2 秒**，代码按样本数判断完成。用于完成校准的那一轮也会跳过写历史。
        - **先校正，再融合，再存档。** `correct()` 复制整份六轴数据，只对 `gx/gy/gz` 各减去自己的零偏。任务随后调用 **`estimator.update(corrected, now_us, attitude)`**。融合有输出时，才把校正后的六轴数据和姿态角一起交给 `history_push()`。
        - **源码** · `components/acquisition/include/gyro_calibration.hpp:25–57`、`components/acquisition/imu_task.cpp:277–334`。

- [x] **S04-B｜单选** 融合里的 `dt` 应该从哪个位置核对？
    - [x] A. 从 `AttitudeEstimator::update()` 看如何处理相邻时间戳
    - [ ] B. 从网页渲染间隔推算
    - [ ] C. 从历史缓存容量推算
    - 我的理由／证据： int64_t delta_us = t_us - previous_us_;
    previous_us_ = t_us;
    - 批阅／二次作答：
        - **判定 · 正确，选 A。** 你引用的 `delta_us = t_us - previous_us_` 就是实测间隔。下一行更新 `previous_us_` 给下一轮使用。
        - **融合使用秒。** `delta_us * 1.0e-6f` 把微秒转成秒，再传给 `FusionAhrsSetSamplePeriod()`。比如 **5,000 µs = 0.005 s**。首个输入只建立时间基准，间隔 ≤ 0 或 > 100 ms 时也跳过当前输出并重置时间基准。
        - **源码** · `components/attitude/attitude.cpp:22–45,79–84`。

- [x] **S04-C｜判断** 启动零偏校正和计算姿态角是同一个步骤。
    - [ ] 对
    - [x] 错
    - 我的理由／证据：
    - 批阅／二次作答：
        - **判定 · 正确，应判错。** 你勾对了，补上两个步骤各自的输出即可。
        - **零偏校准** · 静止时用多个陀螺仪样本估算 `gx/gy/gz` 的固定偏差。
        - **姿态融合** · 用校正后的角速度、加速度和相邻时间戳间隔更新姿态。校准完成后，任务才会沿正常采样路径调用融合。
        - **源码** · `components/acquisition/imu_task.cpp:291–323`、`components/attitude/attitude.cpp:79–88`。

**验收** · 能区分原始读数、零偏校正后的六轴数据和姿态结果，写清输入输出单位与时间含义。

## S05｜历史缓存、序号、批次与丢样

**源码入口** · `components/history/include/sample_record.hpp`、`components/history/history.cpp` 的 `history_push()` 和 `history_copy_after()`；`components/comm/comm.cpp` 的两个发送函数。

- [x] **S05-A｜简答** 说明 `seq`、`t_us`、`after_seq`、`limit` 各代表什么。画出客户端落后到最旧保留记录之前时，`oldest_seq`、覆盖计数和缺口标记如何反映这个情况。
    - 我的答案：每条有效数据的序列号。t_us是当前记录的时间戳。after_seq是下一次开始记录/发送record记录的地方。limit表示这次发送的最大数量。gap表示是否覆盖，这个缺失数量 = overwrite_count;
    - 源码或运行证据：
    - 批阅／二次作答：
        - **判定 · 部分正确。** `seq` 是记录序号，`t_us` 是设备侧微秒时间戳，`limit` 是本批最大条数。
        - **`after_seq` 是排他游标。** 它通常是客户端已经处理到的最后一个序号。代码只复制 **`seq > after_seq`** 的记录。
        - **覆盖计数与缺口分开看。** `overwrite_count` 是这份缓存累计覆盖的记录数。当前查询的 `gap_before_batch` 在**本批有记录且 `oldest_seq - 1 > after_seq`** 时为真。它取决于该客户端的游标。
        - **用项目自测复盘。** 保留区间是 `11…1034`，累计覆盖 **10** 条。`after_seq=0` 时缺口为真。换成 `after_seq=10`，下一条就是 `11`，缺口为假，累计覆盖仍是 **10**。
        - **源码** · `components/history/history.cpp:63–76,102–143,150–208`。

- [x] **S05-B｜单选** 如果客户端每次只拉固定条数，下次请求游标应如何决定？
    - [ ] A. 使用设备当前最新序号，无论本次收到多少
    - [x] B. 基于本批已完整接收并处理的最后序号，并核对 `END`
    - [ ] C. 把上一批的 `limit` 直接用作序号
    - 我的理由／证据：
    - 批阅／二次作答：
        - **判定 · 正确，选 B。** 可以补一句理由。
        - **游标推进到本批真正处理完成的最后一条。** 例如本批返回 `11…42` 且收到 `END 42`，确认该批完整并处理完成后，下次用 **`after_seq=42`**。直接用 `latest_seq` 会跳过尚未读取的记录。
        - **源码** · `components/comm/comm.cpp:49–80`、`components/history/history.cpp:131–143`。

- [x] **S05-C｜判断** 设备在输出整批串口文本期间始终持有历史缓存锁。
    - [x] 对
    - [ ] 错
    - 我的理由／证据：history_copy_after(函数加了ScopedLock锁，这个锁是RAII的，随着函数终止然后释放锁。
    - 批阅／二次作答：
        - **判定 · 需修正，应判错。** 你的 **RAII 推理是对的**，而且正好说明题干为何不成立。
        - **锁持有期间** · `history_copy_after()` 在 `ScopedLock` 的作用域内读取状态并复制记录，退出函数时析构释放锁。
        - **串口发送期间** · `send_samples()` 和 `send_attitude()` 等 `history_copy_after()` 返回后，才用自己的输出数组打印 **`BEGIN → DATA/ATT → END`**。
        - **源码** · `components/history/history.cpp:26–39,84–145`、`components/comm/comm.cpp:30–130`。

**验收** · 能解释空批次、真实缺口和正常分页，以及两种数据流是否应该共享客户端游标。

### S04–S05 追问题

- [x] **S05-F1｜用两个游标区分覆盖与缺口** 历史缓存当前保留 `seq=101…200`，累计 `overwrite_count=100`。客户端甲请求 `after_seq=98, limit=2`，客户端乙请求 `after_seq=100, limit=2`。两边各收到哪些序号？各自 `gap_before_batch` 是什么？如果都完整处理成功，下次请求各用哪个游标？
    - 我的答案：甲没有数据返回，gap = true，乙返回seq=101、102。gap = false,下一次的after_seq = 102
    - 源码或运行证据：batch.gap_before_batch = batch.count > 0 && batch.info.oldest_seq - 1 > after_seq; 
    - 批阅／二次作答：
        - **判定 · 部分正确。** 乙的返回值、缺口和下次游标都答对了。甲也有记录可收。
        - **甲和乙都收到 `101、102`，完整处理后都用 `after_seq=102`。** 缓存从 `101` 开始，复制规则是 `seq > after_seq`，最多两条。
        - **甲的 `gap=true`，乙的 `gap=false`。** 甲缺 `99、100`。乙已处理到 `100`，下一条就是 `101`。你引用的条件要求 **`batch.count > 0`**，所以本例 `gap=true` 恰好说明这批有可返回的记录。证据见 `components/history/history.cpp:131–143`。

## S06｜MCU ↔ PC 串口协议与命令解析

**源码入口** · `firmware/sdkconfig` 的控制台配置；`components/comm/comm.cpp` 的 `comm_task()`、`handle_command()`、`parse_uint64()`、`send_samples()`、`send_attitude()`。

- [x] **S06-A｜简答** 自己整理命令表，写清 ASCII 命令的换行、参数范围、成功与错误响应；分别用 `PING`、`GET_SAMPLES 0 3` 和一条超过缓冲区上限的输入手工走一次解析。指出日志和协议可能在哪交织。
    - 我的答案：PING、CALIBRATE_GYRO、GET_STATUS、SET_RATE hz（100或者200）、GET_SAMPLES after_seq limit（limit:1~history_batch_max）、GET_ATTITUDE after_seq limit（limit:1~history_batch_max）这五大命令。
      PING指令被固件接收之后（comm_task->getchar()->handle_command->send_line("PONG")），通过串口返回PONG。
      GET_SAMPLES 0 3命令，comm_task->getchar()->handle_command->sscanf()->send_sample(0, 3)->history_copy_after给batch和output赋值，获取缓存信息和实际的数据。分别printf BEGIN行，DATA行，END行。
      超过缓冲区上限，在解析参数这一步就会停止，因为limit:1~history_batch_max会有一个if判断。没有进入底层。这个底层本身也会对于limit的大小进行判断
    - 源码或运行证据：
    - 批阅／二次作答：
        - **判定 · 部分正确。** 你列出了 **6 条**命令。`PING` 和 `GET_SAMPLES 0 3` 的调用链也基本对，函数名是 `send_samples()`。
        - **分行规则。** `comm_task()` 逐字符读取，忽略 `\r`，收到 **`\n`** 后给命令补 **`\0`**，再交给 `handle_command()`。输出侧 `send_line("PONG")` 的字面量在内存是 `P O N G \0`。`printf("%s\n", text)` 的 `%s` 读到 `\0` 停止，再由格式串输出 `\n`。
        - **两种超限。** 整条命令最多 **127 个字符**，最后一格留给 `\0`。超过时在 `comm_task()` 丢弃到下一个 `\n`，回复 **`ERR COMMAND_TOO_LONG`**。短命令的 `limit` 超过 **32**，才由 `handle_command()` 回复 **`ERR INVALID_ARGUMENT`**。
        - **成功响应。** `PING` → `PONG`。`GET_SAMPLES` / `GET_ATTITUDE` → `BEGIN`、零或多条 `DATA` / `ATT`、`END`。`SET_RATE 100|200` → `QUEUED <id>`。`CALIBRATE_GYRO` → `CALIBRATION_REQUESTED <id>`。`GET_STATUS` → `STATUS ...`。对应失败可能有 `ERR HISTORY_READ_FAILED`、`ERR BUSY_OR_NOT_READY`、`ERR CALIBRATION_REQUEST_FAILED` 或 `ERR NOT_READY`；未知命令是 `ERR UNKNOWN_COMMAND`。
        - **日志也走输出。** `handle_command()` 先打印 `Received command: ...` 调试行，再发协议响应。PC 应按协议行格式识别真实响应。证据见 `components/comm/comm.cpp:17–27,30–131,137–305`、`components/history/include/history.hpp:10`。

- [x] **S06-B｜单选** PC 从串口一次读到 `PO`，下一次读到 `NG` 和换行。怎样解析？
    - [ ] A. 两次读分别当成两条响应
    - [ ] B. 收到 `PO` 就立即失败
    - [x] C. 保留未结束的字节，等换行后解析完整一行
    - 我的理由／证据：有很多测试的py文件啊，这个是PC解析的文件我都不知道哪个？NG是什么，结束符EOF吗？一般来说我看几个协议都是换行结束的。while b"\n" in pending在ping_test.py中。
    - 批阅／二次作答：
        - **判定 · 正确，选 C。** `NG` 是 `PONG` 的后两个字符。两次读取分别得到 `b"PO"` 和 `b"NG\n"`，拼起来才是 **`b"PONG\n"`**。
        - **你找到的 `ping_test.py` 就是这题的 PC 测试。** `pending.extend(chunk)` 先累积字节，`while b"\n" in pending` 再按换行切出完整行。实际桥接入口也可从 `firmware/test/pc_bridge.py` 的 `read_line()` 调用继续读。证据见 `firmware/test/ping_test.py:21–39`。

- [x] **S06-C｜判断** 一次串口 `read()` 返回的字节必然刚好对应一行协议输出。
    - [x] 对
    - [ ] 错
    - 我的理由／证据：int fields = std::sscanf();
      if (fields != 2这个指令解析会返回这个成功接收的参数的数量。
    - 批阅／二次作答：
        - **判定 · 需修正，应判错。** 一次串口读取可能得到半行、一行或多行。串口分帧由接收缓冲和 **`\n`** 完成。
        - **你对 `sscanf()` 返回值的理解基本对。** 它返回成功匹配并赋值的字段数。固件调用它时，`comm_task()` 已经收齐一行。**`fields != 2` 检查命令字段数**，属于分行之后的参数解析。证据见 `components/comm/comm.cpp:154–161,196–204,263–305` 与 `firmware/test/ping_test.py:21–39`。

**验收** · 能从代码写出 `BEGIN`、`DATA` / `ATT`、`END`、`ERR` 与调试行的处理规则。

## S07｜控制命令：接收、入队、执行、确认

**源码入口** · `components/comm/comm.cpp` 的 `CALIBRATE_GYRO`、`SET_RATE`、`GET_STATUS`；`components/acquisition/imu_task.cpp` 的 `submit_control()`、`imu_task()` 和状态更新。

- [x] **S07-A｜简答** 各追一遍 `SET_RATE 200` 与 `CALIBRATE_GYRO`：参数如何校验、队列由谁写/读、`request_id` 如何获得、状态何时表示实际生效？补充忙、未就绪和失败时如何识别。
    - 我的答案：set_rate和calibrate都是通过调用submit_control(command, request_id)，提交队列xQueueSend(control_queue, &command, 0) != pdPASS;底层，上层是acquisition_request_calibration和acquisition_request_rate做参数校验和command赋值。
      request_id通过引用传递，然后自增全局变量++acquisition_status.request_id;再赋值给request_id。
      读取是imu_task任务处理，通知唤醒后，就会判断是否有队列需要处理。然后分别调用calibration.reset();和esp_timer_restart(sample_timer, period_us);estimator.reset(command.value);然后更新其他的状态变量。
      忙，未就绪和失败我不清楚怎么识别。
    - 源码或运行证据：
    - 批阅／二次作答：
        - **判定 · 部分正确。** 入队、取队列、重启定时器和重置校准器的路径找对了。`request_id` 在入队成功后于锁内递增，再经引用返回。
        - **参数和失败。** `SET_RATE` 校验字段数、无符号整数和 `100/200`，否则回复 `ERR INVALID_ARGUMENT`。`CALIBRATE_GYRO` 走完整字符串匹配，没有速率参数。队列或锁未就绪、已有请求挂起或正在校准时，底层返回 `ESP_ERR_INVALID_STATE`。非阻塞入队失败返回 `ESP_ERR_NO_MEM`。对 PC 端，改频入队失败统一回复 **`ERR BUSY_OR_NOT_READY`**，校准入队失败回复 **`ERR CALIBRATION_REQUEST_FAILED`**。
        - **实际生效看状态。** 改频由 `imu_task` 执行 `esp_timer_restart()`，成功才更新 `rate_hz`，执行结果写入 `last_result`。校准从 `calibration.reset()` 开始，状态先变为 `CALIBRATING`，采够有效样本后才回到 `RUNNING` 并记录 `ESP_OK`。证据见 `components/comm/comm.cpp:162–220` 与 `components/acquisition/imu_task.cpp:125–177,222–270,294–306`。

- [x] **S07-B｜单选** 收到 `QUEUED <request_id>` 后，如何确认采样率已修改？
    - [x] A. 查询 `GET_STATUS`，对照请求标识、挂起状态、执行结果与当前采样率
    - [ ] B. 把 `QUEUED` 当作速率已切换
    - [ ] C. 只看下一条是否出现 `DATA`
    - 我的理由／证据：这个反馈由函数handle_command处理，同时，这个处理最终是通过sub_contorl提交队列到RTOS的，没有保证这个队列的控制信号被接收和执行。这个imu_task执行这个矫正和修改速率的指令后，会同步在锁内更新这个状态变量。看这个信号最稳妥。
    - 批阅／二次作答：
        - **判定 · 正确，选 A。** `QUEUED <id>` 表示**已入队**。
        - **验收条件。** 查询 `GET_STATUS`，核对 **同一个 `request_id`、`pending=0`、`last_result=ESP_OK`、`rate_hz=200`**。如果同一个请求已结束但 `last_result` 是错误码，说明执行失败。证据见 `components/comm/comm.cpp:176–220` 与 `components/acquisition/imu_task.cpp:238–270`。

- [x] **S07-C｜判断** 通信任务一收到 `CALIBRATE_GYRO`，就会同步完成整段零偏校正并返回最终结果。
    - [x] 对
    - [ ] 错
    - 我的理由／证据：被comm_task的handle_command处理->调用acquisition_request_calibration(request_id);提交任务队列，然后imu_task被唤醒后，检查队列的内容做出calibration.reset();矫正处理。
    - 批阅／二次作答：
        - **判定 · 需修正，应判错。** 调用链追对了，结论要改成**异步校准**。
        - **通信任务先回执。** `comm_task` 成功提交队列后就输出 `CALIBRATION_REQUESTED <id>`。`imu_task` 之后取命令并执行 `calibration.reset()`，这是开始校准。
        - **完成要结合模式。** 开始校准时就会清除 `command_pending`，所以还要看 **同一 `request_id`、`mode=RUNNING` 且 `last_result=ESP_OK`**。角速度偏置要累计 **200 个有效样本**才准备好。证据见 `components/comm/comm.cpp:162–191`、`components/acquisition/imu_task.cpp:222–237,294–306` 与 `components/acquisition/include/gyro_calibration.hpp:57`。

**验收** · 能画时序，区分请求已接收、已入队、执行中和执行结束。

## S08｜PC 端：串口字节 → 结构化数据

**源码入口** · 本次检查中 `host/` 为空。使用 `firmware/samples.jsonl`、`firmware/attitude_bridge.jsonl` 对照字段；PC 实际程序入口与协议实现待定位。

- [ ] **S08-A｜简答** 拿一条固件 `DATA` 或 `ATT` 记录，对照一行 JSONL，逐字段写出映射和证据。列出 PC 解析器应该保存什么状态，以及如何处理超时、日志夹杂、重复序号和重连。哪些是已观察到的文件，哪些是你的设计？
    - 我的答案：
    - 源码或运行证据：
    - 批阅／二次作答：

- [ ] **S08-B｜单选** 只有 `attitude_bridge.jsonl` 与其中的 `roll/pitch/yaw` 字段，可以确认什么？
    - [ ] A. 可以确认文件中存在相应字段
    - [ ] B. 可以确认 PC 使用某个 Python 串口库
    - [ ] C. 可以确认这些字段已由 WebSocket 推送
    - 我的理由／证据：
    - 批阅／二次作答：

- [ ] **S08-C｜判断** 断开重连后，如果 MCU 可能重启，PC 继续沿用旧的 `after_seq` 总是安全。
    - [ ] 对
    - [ ] 错
    - 我的理由／证据：
    - 批阅／二次作答：

**验收** · 能说清设备行协议和 PC 结构化记录的边界，给每个尚无源码的结论留证据空位。

## S09｜PC ↔ Web：数据订阅与命令回传

**源码入口** · 本次检查中 `web/` 为空。取得 Web 与 PC 源码后，先查连接入口、数据处理回调、接口路由、命令提交与结果状态。

- [ ] **S09-A｜简答** 画出 PC 数据进网页、网页命令回到 MCU 的两个时序。分别填写谁发起连接、真实传输方式、消息字段、请求关联、超时和断线恢复。未见源码时标为待确认，并写出第一份应找的证据。
    - 我的答案：
    - 源码或运行证据：
    - 批阅／二次作答：

- [ ] **S09-B｜单选** 要确认 PC 到网页用 HTTP、SSE 还是 WebSocket，优先找什么证据？
    - [ ] A. 开发板的 Type-C 接口
    - [ ] B. 浏览器网络记录与两端接口实现
    - [ ] C. JSONL 文件的扩展名
    - 我的理由／证据：
    - 批阅／二次作答：

- [ ] **S09-C｜判断** 浏览器能收到实时数据，就可以据此判断它使用了 WebSocket。
    - [ ] 对
    - [ ] 错
    - 我的理由／证据：
    - 批阅／二次作答：

**验收** · 能画出真实 PC↔Web 接口；拿不到实现时，明确区分已验证链路和待确认链路。

## S10｜端到端联调与故障定位

**源码入口** · 从 `firmware/main/main.cpp`、`components/comm/comm.cpp`、两份 JSONL 到实际 PC 日志、浏览器网络记录。

- [ ] **S10-A｜简答** 挑一条 `ATT` 数据，按 `seq` 关联固件输出、PC 解析结果、页面显示；再挑一次 `SET_RATE` 或校准命令，按请求标识关联入队和最终结果。写出每一跳的证据和当前缺口。
    - 我的答案：
    - 源码或运行证据：
    - 批阅／二次作答：

- [ ] **S10-B｜单选** 设备端已经有完整 `ATT` 行，但网页数值不动。第一轮应比对什么？
    - [ ] A. 先更换开发板型号
    - [ ] B. 同一条记录是否进入 PC 解析结果，再看 PC→Web 消息与页面处理
    - [ ] C. 先增大陀螺仪量程
    - 我的理由／证据：
    - 批阅／二次作答：

- [ ] **S10-C｜判断** 仅用一次 `PING` 得到 `PONG`，就能验收校正数据、批次完整性和网页命令回传。
    - [ ] 对
    - [ ] 错
    - 我的理由／证据：
    - 批阅／二次作答：

**验收** · 分别记录正常数据、一条错误命令、缓存缺口和中断重连的结果，标明已实测到哪一层。

## 联调记录表

下面是可填写的记录。涉及串口的例子来自当前固件命令处理逻辑，运行结果留待实测。

| 检查项 | 从哪一端观察 | 证据或结果 |
|---|---|---|
| `PING` 请求与 `PONG` 响应 | MCU 串口、PC 接收日志 | 待填 |
| `GET_ATTITUDE 0 3` 批次 | MCU 串口、PC 解析日志 | 待填 `BEGIN`、`ATT`、`END` |
| `SET_RATE 200` 执行 | 入队回应、`GET_STATUS`、样本间隔 | 待填请求标识、最终状态与实测间隔 |
| 陀螺仪校正 | 状态、校正前后静止样本 | 待填零偏与完成证据 |
| 缓存覆盖、重连或 MCU 重启 | 序号、缺口、客户端游标 | 待填 |
| PC 到 Web 的数据及反向命令 | PC 日志、浏览器网络记录 | 待定位实现 |

## S01｜工程身份与构建边界

**建议源码入口【通用参考】**：README、CMakeLists.txt、main/、components/、idf_component.yml、dependencies.lock、sdkconfig、sdkconfig.defaults、分区表；Arduino 项目查看 .ino、libraries/、platformio.ini。

- [ ] **S01-A｜简答** 当前真正编译的是哪个目录和固件目标？写出你确认入口、框架及版本所依据的文件。
    - 我的答案：main是主程序入口，components是我自己写的xx文件【库】？idf_component.yml是第三方安装的组件，注册到这个esp32的程序中。
    - 源码或运行证据：
    - 批阅／二次作答：**入口**是**main/app_main()函数**。框架本地所需的文件是**sdkconfig,这个文件记录了配置信息“esp32s3”**。**构建产物是firmware.bin**,在firmware/build/firmware.bin。

- [ ] **S01-B｜简答** 选一个看起来相关的驱动，说明它为什么会进入这个固件，或为什么没有进入。
    - 我的答案：ii2的驱动。为什么进入固件？因为要iic读取imu数据到esp32啊。什么叫做进入这个固件？
    - 源码或运行证据：
    - 批阅／二次作答：为什么会进入固件？**函数调用链路**：app_main()函数的board_i2c_init()函数，这个函数调用了i2c_new_master_bus()。**构建结果：firmware.map有`i2c_new_master_bus` 的链接地址（.a的中间产物）。**

- [ ] **S01-C｜单选** 仓库里同时出现 .ino 和 CMakeLists.txt。什么证据最能确定板子当前烧录的固件来源？
    - [ ] A. 目录中最早出现的文件
    - [x] B. 实际构建命令、构建目标与烧录记录的对应关系
    - [ ] C. 文件数量最多的示例目录
    - 我的理由／证据：这个终端会显示烧录固件的地址啊，但是，这个地址我没有关注过欸。要是让我来看这个烧录信息我都不一定直到这个信息。
    - 批阅／二次作答：/D:/Git/esp-project/esp32-attitude-monitor/firmware/build/flash_args这个文件，记录了烧录参数。**0x10000 firmware.bin这个是明确证据，这个包含构建产物和flash烧录地址**

- [ ] **S01-D｜判断** 一个驱动文件只要存在于仓库里，就一定会进入当前固件。
    - [ ] 对
    - [x] 错
    - 我的理由／证据：不是，需要注册进这个esp32的官方程序中，因为是CMake构建，所以要看这个CMakeLists.txt的信息。但是看哪一个呢？D:\Git\esp-project\esp32-attitude-monitor\firmware\CMakeLists.txt还是D:\Git\esp-project\esp32-attitude-monitor\firmware\main\CMakeLists.txt？
    - 批阅／二次作答：

**本节验收**：能准确说出正在研究哪个固件，留下可复现的构建步骤；没有源码时保持待确认。

> [!note]- S01 深入阅读提示（选做）
> - [ ] S01-01：正在阅读的是独立应用、官方单项示例，还是多个示例的集合？实际构建哪个子目录？
>       是独立的应用，但是使用了第三方的库。所以也可以说是多个示例的集合？实际构建的是firmware这个子文件夹。
> - [ ] S01-02：仓库地址、分支、提交号和本地修改是什么？后续证据能否固定到这个版本？
>       仓库是：分支是main分支，没有独立的开发分支再合并进main分支。
>       31657f0fe211992283de7eebb3c2fa46af30dd2d这个是第一个初始化的提交号。
> - [ ] S01-03：实际采用 ESP-IDF、Arduino，还是 Arduino 作为 IDF 组件？SDK、Core 和关键依赖版本如何确认？
>       实际采用ESP-IDF，因为这个插件就是ESP-idf,而且这个终端的信息也明确来着ESP-IDF。**CMakeLists.txt也引入了ESP-IDF的project.cmake**
>       sdk,core和关键版本是什么？为什么要确认？有什么用？
> - [ ] S01-04：目标芯片、板卡修订版、Flash、PSRAM、分区和控制台配置在哪里定义？
>       芯片ESP32-s3,板卡 = 开发板（屏幕+IMU+Type-C）？有一个sdk编辑器可以修改很多配置。插件里也可以修改。最终的配置文件是`sdkconfig`但
> - [ ] S01-05：哪些源文件真正加入构建，哪些是旧文件或未启用示例？条件编译最终选择哪套实现？
>       构建看build文件夹，build/complie_commands.json文件有真实完整的编译指令。
> - [ ] S01-06：能否按现有说明构建、烧录并看到启动日志？需要哪些工具版本和配置？
>       配置，构建，编译，烧录，监视都可以通过插件完成。配置阶段：通过CMakeLists.txt，CMake生成Makefile文件，构建阶段：把xx编译成，然后链接依靠xx

### S01-05 追问订正

你找对了两份证据。先看 `firmware/components/board/CMakeLists.txt` 的 `idf_component_register(SRCS ...)`，确认组件声明了哪些源文件。这里明确列的是 `board_i2c.cpp`。假设只把 `foo.cpp` 放进目录，仍需把它加入 `SRCS` 才能通过这条声明参与构建。

再打开 `firmware/build/compile_commands.json`，搜索源文件名。正确拼写是 **`compile_commands.json`**，其中 `file` 是源文件，`command` 是编译命令，`output` 是目标文件。它记录的是 CMake 为当前构建配置生成的命令。要确认某次构建确实生成了目标文件，再结合构建日志与对应的 `.obj` 文件。要确认某个函数进入最终应用，可以查 `firmware/build/firmware.map`。

### S01-06 参考作答与项目实读

我可以在 ESP-IDF 插件里修改项目选项、执行构建、烧录和打开串口监视器。本项目根目录是 `firmware/`，构建输出目录是 `firmware/build/`，芯片目标是 `esp32s3`。**【配置阶段】**，CMake 读取项目与组件的 `CMakeLists.txt`，ESP-IDF 根据 Kconfig 和 `sdkconfig` 处理配置项，最后生成 Ninja 构建规则。
**【构建阶段】**，**【编译环节】**：Ninja 按规则调用 GCC/G++ 把源文件编译成 `.obj`，组件目标文件归档成 `.a`，**【链接环节】**：再由交叉 G++ 驱动链接器生成 `firmware.elf`。随后 `esptool elf2image` 生成 `firmware.bin`。烧录后用 Monitor 看启动输出和应用版本。现有文件可以核对配置和构建产物，实际烧录成功与启动日志需要板端记录。

下面的路径都相对于仓库根目录 `D:\Git\esp-project\esp32-attitude-monitor`。

#### 第一环节 · 配置与生成构建规则

打开插件的 SDK Configuration Editor，或者在 ESP-IDF 终端运行 `idf.py menuconfig`，可以修改诸如目标相关选项、控制台和 Flash 参数。执行 Build 时，`idf.py` 会按需运行 CMake。这里的「配置」既涉及**选项值**，也涉及**把这些选项和源码声明变成可执行的构建规则**。

| 阅读顺序 | 打开哪个文件 | 这一步确定什么 |
|---|---|---|
| 1 | `firmware/CMakeLists.txt` | `include($ENV{IDF_PATH}/tools/cmake/project.cmake)` 接入 ESP-IDF，`project(firmware)` 定义工程名。这里的 `3.22` 是 CMake 最低要求。 |
| 2 | `firmware/main/CMakeLists.txt` → `firmware/components/board/CMakeLists.txt` | `main` 把 `main.cpp` 登记进 `SRCS`，并依赖 `board` 等组件。`board` 把 `board_i2c.cpp` 登记进 `SRCS`，并依赖 `esp_driver_i2c`。 |
| 3 | `firmware/main/idf_component.yml` → `firmware/dependencies.lock` | 前者声明 QMI8658 组件的版本范围，后者记录解析结果，这次是 QMI8658 `2.0.2`，ESP-IDF `6.1.0`。 |
| 4 | `firmware/sdkconfig` → `firmware/build/config/sdkconfig.h` 和 `sdkconfig.cmake` | `sdkconfig` 保存工程选项。以 `CONFIG_IDF_TARGET="esp32s3"` 为例，生成的 `sdkconfig.h` 有对应的 C/C++ 宏，`sdkconfig.cmake` 给构建逻辑读取配置。 |
| 5 | `firmware/build/CMakeCache.txt` → `firmware/build/build.ninja`、`firmware/build/CMakeFiles/rules.ninja` | 缓存里实际是 `CMAKE_GENERATOR:INTERNAL=Ninja`。所以你原答案里的「生成 Makefile」在本项目要改成「生成 Ninja 构建文件」。`build.ninja` 描述输入、输出和依赖，`rules.ninja` 给出执行命令的规则。 |

读到这一步，你可以在 `build.ninja` 搜索 `board_i2c.cpp.obj:`。那一行把源文件、目标文件和 C++ 编译规则对应起来。要看 CMake 如何把声明变成步骤，这比只看插件按钮更直接。[ESP-IDF 构建系统](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/build-system.html)

#### 第二环节 · 用一个文件追完编译和链接

1. **编译**。打开 `firmware/build/compile_commands.json`，搜 `board_i2c.cpp`。这里安排 `xtensa-esp32s3-elf-g++.exe` 编译 `firmware/components/board/board_i2c.cpp`，输出 `firmware/build/esp-idf/board/CMakeFiles/__idf_board.dir/board_i2c.cpp.obj`。我核对到这份 `.obj` 已存在。
2. **归档**。在 `firmware/build/build.ninja` 搜 `build esp-idf/board/libboard.a:`，能看到 `board_i2c.cpp.obj` 是输入。归档这一步把组件的目标文件收进静态库，供后面的应用链接使用。
3. **链接**。在 `firmware/build/build.ninja` 搜 `build firmware.elf:`，再到 `firmware/build/CMakeFiles/rules.ninja` 找它引用的 `CXX_EXECUTABLE_LINKER__firmware.2eelf_`。该规则通过 `xtensa-esp32s3-elf-g++.exe` 调用链接器，把所需的目标代码和库链接为 `firmware/build/firmware.elf`，并按链接脚本布局各段。例如 `main.cpp` 调用 `board_i2c_init()`，对应的实现来自 `libboard.a`。可以在 `firmware/build/firmware.map` 搜这些符号核对链接结果。
4. **生成镜像**。在 `firmware/build/build.ninja` 搜 `elf2image`，能看到 `esptool` 读取 `firmware.elf` 并输出 `firmware.bin`。`.elf` 含符号等信息，供调试和定位问题；`.bin` 是烧录用的应用镜像。

```text
board_i2c.cpp → board_i2c.cpp.obj → libboard.a
                                       ↓ 和其他应用组件一起链接
                                  firmware.elf → firmware.bin
```

Ninja 会依据依赖和文件变化安排增量构建。比如只改 `board_i2c.cpp`，通常可以看到该文件重编译，接着重新归档、链接和生成镜像。改了 `CMakeLists.txt` 或项目配置，通常还会先重新配置，实际步骤以终端输出为准。

#### 工具版本与烧录验收

- **现有构建记录** · ESP-IDF `6.1.0`，CMake `4.0.3`，Ninja `1.12.1`，Xtensa 工具链安装包 `esp-15.2.0_20251204`。分别可从 `firmware/dependencies.lock`、`firmware/build/CMakeCache.txt`、`firmware/build/compile_commands.json` 核对。这是现有构建使用的版本。
- **项目配置** · 从 `firmware/sdkconfig` 核对目标 `esp32s3`、Flash `DIO / 80 MHz / 2 MB`、分区 `partitions_singleapp.csv` 和 USB Serial/JTAG 控制台。Flash 容量这里是工程配置值，实物规格另行核对。
- **复现流程** · 在 ESP-IDF 终端切到 `firmware/`，按需运行 `idf.py menuconfig` 或 `idf.py reconfigure`，再运行 `idf.py build`。用插件选择实际 COM 口，或运行 `idf.py -p COMx flash monitor`，把 `COMx` 换成实际端口。
- **验收记录** · 构建时核对 `firmware.elf` 和 `firmware.bin`。烧录时对照 `firmware/build/flash_args` 中的 `0x0`（bootloader）、`0x8000`（分区表）和 `0x10000`（应用），再保存写入成功的终端记录。监视时在重启后记录应用版本和启动输出，并与 `firmware/build/project_description.json` 的项目版本对照。本次核对到本地构建产物，烧录与板端启动待实际验证。[ESP-IDF 命令说明](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/tools/idf-py.html)

