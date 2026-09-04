# ST1C6b3 Schema 根类型准入

## 范围与状态

承接 [C6b 声明账本](ST1C6b-公共契约逐项审计.md)，基线 e1aa559。本节点只修复基础 Schema 根类型的未知枚举准入及 jsoncons 的降级分支，不新增上层模块、公共 API 或依赖。真实负向两项 0/2；修复后相关 20 项各三次，扩大 Debug/Release 选集各 147/147、ASan 同选集各三次 441/441、探针另 3/3 及生产/架构通过，见 [交付记录](../阶段交付/2026-09-05-ST1C6b3-Schema根类型准入.md)。本地检查点成立，不代表完整 C6 或 Kernel Frozen。

## 准入规则

1. SchemaKind 的底层类型仍为 uint8_t，合法值仅 Any、Null、Boolean、Integer、Number、String、Array、Object 八个具名枚举。Schema::create 先做显式允许列表检查；8..255 的全部未定义值返回 Validation/Foundation.SchemaKindInvalid，不先用其他元数据错误遮蔽根类型错误。
2. 已定义根类型继续执行原 constraints:Object 与 unit 非空/非全空白规则；本节点不将 constraints 的形状检查扩张为完整 JSON Schema 编译认证。合法 id/version/constraints/unit 和原枚举值均保持不变。
3. 只有显式 Any 可以省略后端根 type。Any 的 constraints 仍然参与验证，不等于无条件接受所有值；例如 constraints 自带 type/minimum 时仍应拒绝不符合者。
4. jsoncons 的 schemaType 默认分支改为抛 invalid_argument，validate 的既有异常边界将其映射 Serialization.SchemaBackendFailed。正常公共工厂已经阻止此值进入；该分支是内部防御，不为测试伪造私有 Schema、修改对象内存或新增旁路构造器。

## 证据与兼容性

新增四个 CTest：Foundation 穷举 248 个未知值，各检查正常/错误其他元数据共 496 次工厂调用；八个合法值的元数据保持；两个未知代表值的工厂/真实后端回归；8 种合法 SchemaKind × 7 类 Value 的 56 对类型矩阵，以及三个显式 Any 约束场景。Number 接受 Integer 保持既有语义，Integer 与非整数 Number 区分；该矩阵不宣称覆盖所有数值或 JSON Schema 关键字。

红灯时期未知 8、255 可构造并在真实 jsoncons 中通过七类 Value，证明此前等同于无 type 的 Any，而不仅是静态代码猜测。修复后工厂失败，测试不再向后端送入无法合法构造的未知 Schema；后端防御分支未通过不安全对象伪造直接覆盖，证据范围明确保留。

这属于非法输入行为收紧，新增错误码 Foundation.SchemaKindInvalid；不改变 71 个公共头、SchemaKind 数字、对象 schemaVersion 或持久数据库版本。Workflow 定义摘要记录的合法 kind 数字保持不变；本节点不将非法旧定义转换成 Any，也不提供修复旧存储材料的入口。C6b 仍须完成其他 wire/错误兼容审计。

本节点不是任意内存耗尽下的 noexcept、跨工具链 C++ ABI、全部 Schema 合法性或资源预算认证。递归深度、字节/节点/数量上限、UTF-8/非有限数值、编译成本和取消语义仍归 C6b/c/C7；消息未知枚举、跨版本合并与订阅寿命仍按后续计划执行，整体尚未 Frozen。
