# K10E E1A 对象类型基础组件

## 状态与范围

E1A 是 K10E 的内部可独立验证节点，不代表 K10E 已验收。当前只实现对象类型注册、版本校验、显式迁移和对象引用枚举；尚未接入 AppKernel/ModuleRegistrar、Document、Journal、History 或 AssetStore 的准入链。

## 类型定义

`ObjectTypeDefinition` 包含：

- `ObjectTypeDescriptor`：稳定 ObjectTypeId、当前 Schema Version、`Durable` 或 `Transient` 持久化策略；
- `ObjectTypeVersion`：精确版本、只读 Validator、只读 Reference Enumerator；
- `ObjectTypeMigration`：源版本、目标版本与只读 Migration 回调。

每个类型由一个完整定义原子注册。当前版本必须存在，每个旧版本都必须有显式路径通往当前版本；迁移只允许前向、已注册端点和唯一出边。重复版本、反向边、缺失路径和空回调均拒绝。

`Transient` 在本节点只是一项已校验的契约声明；后续持久化准入必须显式拒绝把该类对象写入持久 Document，不能静默丢弃。

## 校验与迁移

`validate(type, version, data)` 只使用精确版本的 Validator。未知类型或版本拒绝，不自动猜测或升级旧 Value。

`migrate(type, from, to, data)` 先预检完整路径，再校验源数据，依次执行每个迁移并按目标版本重新校验。禁止降级；同版本迁移仍校验数据后返回副本。任何失败均保留调用方原 Value，不产生部分迁移结果。

迁移回调必须是确定、无外部副作用的数据转换。它不具备 Document 写入、Revision 推进、Journal 追加或 History 操作权限；后续应用迁移结果必须走唯一内核事务链。

## 引用与并发

Reference Enumerator 按精确版本读取对象数据，输出同一 Document 内的稳定 ObjectId；Registry 规范化为排序去重集合。本节点只枚举引用，悬空引用校验由后续 Document 准入集成完成。

所有 Validator、Migration、Reference Enumerator 均在 Registry 锁外执行，允许只读重入。回调实现必须自身支持并发只读调用。返回错误和抛出异常统一转换为 ObjectType 错误，不传播异常穿透内核。

## 验证

- Debug/Release：171/171 CTest 通过；
- 新增 6 个对象类型用例，重复 20 次共 120 项通过；
- 覆盖确定性目录、冻结、未知类型/版本、完整迁移链、非法图、源/中间结果校验、错误/异常隔离、源 Value 不变、引用规范化、锁外重入和并发读取；
- 架构扫描：65 个公共头文件、126 个生产源文件通过。

## 下一节点

将 ObjectTypeRegistry 纳入 ModuleRegistrar 声明/所有权/回滚和 ExecutionGateway 发现；为 ObjectRecord 持久化精确 Schema Version，并在事务提交、文档打开与恢复时执行类型和引用准入。之后再闭合 AssetRef/AssetStore 与 OCCT 架构边界。
