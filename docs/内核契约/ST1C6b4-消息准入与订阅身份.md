# ST1C6b4 消息准入与订阅身份

## 范围

基线 245ea2a。修复 EventBus 未知枚举准入、Notification 跨版本合并及取消后 SubscriptionId 复用的旧消息错投；不扩展上层模块，不改变公共头、持久格式或依赖。五项真实红灯 0/5，专项 12 项各十次、Debug/Release 选集各 161/161、ASan 同选集三次 483/483、探针另 3/3 及纯生产/边界通过，见 [交付记录](../阶段交付/2026-09-05-ST1C6b4-消息准入与订阅身份.md)。本地检查点成立，完整 C6–C8/ST1D 尚未签核。

## 准入、版本与身份

1. subscribe 保留空 callback 的 Event.InvalidSubscriber 拒绝；随后只允许 Immediate/Queued，未知 DeliveryMode 返回 Validation/Event.InvalidDeliveryMode。可选 filter.kind 只允许 Domain/Notification/System，未知值返回 Validation/Event.InvalidFilterKind。检查在注册表锁和插入之前，不留下订阅或新增排队状态。
2. 合并只适用于 Queued Notification：同一订阅实例、EventName、完整 Version（major/minor/patch）及存在且相等的 coalescingKey 才可替换。不同版本不做隐式兼容合并，Domain/System 仍不合并。相同版本替换保持原队列位置和新 payload。
3. 每次成功注册拥有一个私有不可变 SubscriptionIdentity。排队项共享其生命周期；drain 查到的当前订阅必须同时匹配公开 ID 和实例标识。取消后复用相同 SubscriptionId 不能继承旧项，旧 filter/mode 的消息也不能迁移到新 callback。
4. 标识对象只用于进程内实例区分，不是持久 ID 或访问凭据。未结束排队项持有原标识，因此分配器地址复用不能产生同一存活标识，且没有递增计数回绕。标识不持有 callback，不延长已取消订阅捕获资源的寿命。
5. 上述检查同时覆盖仍在总队列中的项和已提取到局部 drain 批次的项；仅在 cancel 时清理总队列并不足以证明后一情况。drain 的 matched 仍统计取出的候选数，delivered 只统计实际成功回调；失效候选可使二者不同。

## 取消和资源边界

cancel 不是在途回调的 join。Immediate 的投递快照或 Queued 已通过实例核对并复制的 callback 可在取消后执行，但不会改交给新订阅的 callback。使用者必须让其捕获资源覆盖这些调用的寿命，不得把取消返回当作所有回调已排空。对同一个 EventSubscription 对象并发 move/cancel 不在本节点保证范围。

失效排队候选可以保留到显式 drain，queuedCount 不等于仍可投递的数量。本轮不为满足容量目标静默改写 matched、清除活动、丢弃有效消息或复用旧身份；总队列/订阅/Value 字节预算仍归 C6c/d。私有标识增加每个注册实例的分配和排队项共享引用成本，publish 的既有全队列复制仍保留，须由 C7 实测。

本轮源码复核还发现 cancel/Bus 析构会在内部锁内销毁注册项的 callback；捕获资源析构重入、回调副本复制/分配失败和 drain 异常边界尚需下一子节点实测。不把“调用 callback 在锁外”扩张为“所有用户定义复制/析构均在锁外”。这不是当前回归已证明通过的行为。

## 覆盖和兼容

新增八项：254 个非法 mode，以及 253 个非法 filter kind × 两种合法 mode，共 760 次拒绝；major/minor/patch 三类版本隔离；两种新订阅模式下旧项不能复活；局部 drain 批次重入、跨订阅实例合并拒绝；八种合法 mode/filter 组合；真实双线程取消/重注册与 drain；已捕获 Immediate 回调的旧归属保持。正例使用真实 CommittedDomainEvent 与受限 Transient 工厂，不伪造 Domain 事实。

非法枚举现在提前失败，旧跨版本合并将保留两个消息而非丢失一个版本，取消后旧消息不再错投给同名新订阅；这是行为兼容收紧。公共 API、Error DTO 和持久版本不变，新增两个 Event 错误码。Domain 消息观察仍不能改变事务/History 真值，不提供可靠消息队列或自动重放保证。
