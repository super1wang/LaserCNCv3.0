# Kernel 1.0 公共头清单（C6 起始基线）

## 使用范围

基线来自 C6a 当前工作树的 `include/lasercnc`，共 71 个公共头；本轮路径修复未修改这些头。下表逐文件 SHA-256 用于后续差异核对，不表示 API 已冻结、每个 DTO 字段已签核或跨工具链 ABI 已稳定。公开组件的独立构造能力不等于 AppKernel Host 可以绕过 ExecutionGateway，权限/所有权仍须逐入口审查。

C6b 必须依据实际声明补齐类/函数/DTO/错误码及持久格式兼容清单，不能用文件数和摘要代替语义审计。后续任何公共头修改均需更新基线及兼容说明；最终冻结基线只能在 C6–C8/ST1D 验收后签发。当前执行顺序见 [C6 契约](ST1C6-公共契约与输入预算.md)。

## 目录分布

| 目录 | 公共头数 |
| --- | --- |
| foundation | 7 |
| infrastructure | 8 |
| kernel | 7 |
| messaging | 2 |
| observability | 5 |
| persistence | 1 |
| platform | 6 |
| runtime | 29 |
| state | 6 |

## 逐文件清单

| 公共头 | SHA-256 |
| --- | --- |
| [foundation/error.hpp](../../include/lasercnc/foundation/error.hpp) | `F028ACCEF124F095A70C96182D92D8A6DF9BDD37856E1C9FF8E9530440D0AF26` |
| [foundation/result.hpp](../../include/lasercnc/foundation/result.hpp) | `B46264DDEDB47BC618352BAE40A5865E45A03F6FB8F27CE714BD67B957574203` |
| [foundation/schema.hpp](../../include/lasercnc/foundation/schema.hpp) | `7B509F80B0FC868BB9D0A64D60CD21479A9320391FD4BB340CA9A8A536730A5C` |
| [foundation/serialization.hpp](../../include/lasercnc/foundation/serialization.hpp) | `BB092B5775B2B2242A94312F6A295E886911F5EAD609CA0763644091ED14F5C2` |
| [foundation/strong_id.hpp](../../include/lasercnc/foundation/strong_id.hpp) | `6C1B8317EDD98026E562012759F16D4ED8DDC0BCBF96E78944C8B75D53CAAC25` |
| [foundation/value.hpp](../../include/lasercnc/foundation/value.hpp) | `BE0A6B38F7624654DE3011755B0C371A48E2FBA341BA80405CD00EF93ECC30B0` |
| [foundation/version.hpp](../../include/lasercnc/foundation/version.hpp) | `DFFD171AA3005DF4F4ACFB9BEDA896970723D65B10042434E08C32E777087684` |
| [infrastructure/bs_thread_pool_executor.hpp](../../include/lasercnc/infrastructure/bs_thread_pool_executor.hpp) | `FC7E7C17EC78AEFFC644BA87FFE6D0AB986D3742C3E56A475429CB7B4EDEC557` |
| [infrastructure/filesystem_asset_store.hpp](../../include/lasercnc/infrastructure/filesystem_asset_store.hpp) | `28262DB241FF9F6246E5A16A20433A307C1EC8240744EF63D8CCC40D04A8EB7C` |
| [infrastructure/filesystem_snapshot_store.hpp](../../include/lasercnc/infrastructure/filesystem_snapshot_store.hpp) | `D1701833E94A3A29D8EC58E4B325314AF9A89560917437C2EA486180F5832DF7` |
| [infrastructure/jsoncons_adapter.hpp](../../include/lasercnc/infrastructure/jsoncons_adapter.hpp) | `FD7E5FEAF5957ADEE2AE1C7949B384F3F7365700354052199E264E5AA018A1F0` |
| [infrastructure/sha256_hash_service.hpp](../../include/lasercnc/infrastructure/sha256_hash_service.hpp) | `6684E00B2EE577CA2AD11C29F9B3DC3D71540FB6DAD2F4DCA3278D66F1BC2C8C` |
| [infrastructure/spdlog_log_service.hpp](../../include/lasercnc/infrastructure/spdlog_log_service.hpp) | `5F9937026F04C2AFFCE9856D3DFA27B2CC2516C44A5AF91062D8357B73CB4F5E` |
| [infrastructure/sqlite_persistence_backend.hpp](../../include/lasercnc/infrastructure/sqlite_persistence_backend.hpp) | `0949C064697974FAE96F83C387C7B88F927BF8BD055E4FACCB8AFB11A1035891` |
| [infrastructure/toml_config_adapter.hpp](../../include/lasercnc/infrastructure/toml_config_adapter.hpp) | `EDF01E5D7BC558FC319E40D889C355E8D161BC61BA8505B7DE820FBB0F3F0C60` |
| [kernel/app_kernel.hpp](../../include/lasercnc/kernel/app_kernel.hpp) | `5A8A38EF3B3E82E5F1A787C61849F86680205EE72CCC9DB371D4C69E28B40549` |
| [kernel/execution_gateway.hpp](../../include/lasercnc/kernel/execution_gateway.hpp) | `CFE4CC4BF492CAB889296C4AE6C1E98E84926452A676AA430EEF90C3A2C444F9` |
| [kernel/identifiers.hpp](../../include/lasercnc/kernel/identifiers.hpp) | `2F57550122E743881626719E42711A037D6E934177997B618FF3514C72843CB0` |
| [kernel/module_registrar.hpp](../../include/lasercnc/kernel/module_registrar.hpp) | `026B759600C7CA547A82A7B10B9AA7016CB96ADC6123E8C42E037F91B20E4E18` |
| [kernel/module_runtime.hpp](../../include/lasercnc/kernel/module_runtime.hpp) | `747B04ADA89F60AE6D8F5E0E042E9D0EF17E1E6E4F4F3CFC97344E54479530B2` |
| [kernel/module.hpp](../../include/lasercnc/kernel/module.hpp) | `A45DDC271B564C68781AF61F6BDE223A6FB10E70D156B549382DE6CA532026FF` |
| [kernel/service_registry.hpp](../../include/lasercnc/kernel/service_registry.hpp) | `032099BEC9002B6BFA4EADA74E2CFA5F53A02A35B73C12CCA3CF520DAC76D062` |
| [messaging/domain_event.hpp](../../include/lasercnc/messaging/domain_event.hpp) | `3DF2E34475829A5122B4481AEF44B2A06D283FDA7EF7642A6F5404AA28914243` |
| [messaging/event_bus.hpp](../../include/lasercnc/messaging/event_bus.hpp) | `BAA719792D04A74CF763390FC6535515EB0B0FE5D264C1B7BD1F4C0005B32945` |
| [observability/diagnostics_service.hpp](../../include/lasercnc/observability/diagnostics_service.hpp) | `75EC19DE0ADE38370C13214D4D19F5DDAA31E26E820B4B363F9C4A871E0C65B4` |
| [observability/log_observability_exporter.hpp](../../include/lasercnc/observability/log_observability_exporter.hpp) | `E3EAB59D2D36C73C52AF0B4AFE213AF8B09A14304561B6CB5B633BCC2F23354E` |
| [observability/log_service.hpp](../../include/lasercnc/observability/log_service.hpp) | `85C5DCBCB14ABA2673FFAF77FA52BD75E3E2912194C05819B72B975628DD3184` |
| [observability/metrics_service.hpp](../../include/lasercnc/observability/metrics_service.hpp) | `CAED2D253FFD4B11B932140D0A7A263918ACE695CE1E28377136E2CBF7BBF51C` |
| [observability/trace_service.hpp](../../include/lasercnc/observability/trace_service.hpp) | `F57BF1CB446EEC287C84D1FEFDABF76A3A62738DFB004C2F196DB8960745F830` |
| [persistence/persistence_service.hpp](../../include/lasercnc/persistence/persistence_service.hpp) | `8BEE0C5FF433E68FE69D8FE2FDEA4915238FC58E46534E760C1F2D2FFB0879D3` |
| [platform/asset_store.hpp](../../include/lasercnc/platform/asset_store.hpp) | `8FD9FECC392DFA4F9E2CB8992D4068B83EBD64976E9D50442D27F75C83D804F3` |
| [platform/config_serializer.hpp](../../include/lasercnc/platform/config_serializer.hpp) | `CE2C8A06A18225D6E4C5B9D90FC9CB775A964ABC2156786254D0FAE777579A98` |
| [platform/hash_service.hpp](../../include/lasercnc/platform/hash_service.hpp) | `D8F7C090D4751B92B7D9F64F87CC00E6F5A3980E257B1D6906B6B66BA8E65D5C` |
| [platform/persistence_backend.hpp](../../include/lasercnc/platform/persistence_backend.hpp) | `9DDFBB8B17E21E7DA5496F10B30FD575122A9F03E4113F8431CEEFEDC98F665C` |
| [platform/snapshot_store.hpp](../../include/lasercnc/platform/snapshot_store.hpp) | `73F450C75B0985D8FD692F0E011F20D96C7DD3D8DDF26F42E3C6D504C06BC96D` |
| [platform/task_executor.hpp](../../include/lasercnc/platform/task_executor.hpp) | `26BFC72C4FA6E498F336539C0248AD374DF8C8D753591EB87E501BF571AD2627` |
| [runtime/asset_validation.hpp](../../include/lasercnc/runtime/asset_validation.hpp) | `690A18FD72E7BC114CA5231A67CC7F4A73737BBBF3809C39F93E06EEDF681BF3` |
| [runtime/capability_service.hpp](../../include/lasercnc/runtime/capability_service.hpp) | `5D30C8F1D1AE696030F38B2DC9E975D383AD4B1F9A1BDFBFDD2D1AAC34A00FEC` |
| [runtime/command_registry.hpp](../../include/lasercnc/runtime/command_registry.hpp) | `C98A0701A3F26F566D332E9D144D05348BD297B36231EBC9196D6521D07F1ABD` |
| [runtime/command_runtime.hpp](../../include/lasercnc/runtime/command_runtime.hpp) | `3315E73CE97E2C08CD17EAD891A096F89C46AC9C18F133F3CAF7E41F037E6891` |
| [runtime/command.hpp](../../include/lasercnc/runtime/command.hpp) | `8EE7F14739DE9B6A64822282776962E4F2711FD96A072659BC4B467EC3E58A13` |
| [runtime/document_runtime.hpp](../../include/lasercnc/runtime/document_runtime.hpp) | `7E348A86898B31A1D6E43E32A08C926ED98CBD2A44165CAB2FB2BD4214F450BE` |
| [runtime/effect_executor.hpp](../../include/lasercnc/runtime/effect_executor.hpp) | `29A051057C053B581F098F08DF74F6823D2E941251032C42EC9F7E900EE9058C` |
| [runtime/effect_guard.hpp](../../include/lasercnc/runtime/effect_guard.hpp) | `57AB275A9A54F146657D86A8A08ED3652926CB0ACBBA5422DF9ED798786412C1` |
| [runtime/execution_contract.hpp](../../include/lasercnc/runtime/execution_contract.hpp) | `DB2142637E5F3514AA84C2EA81C39BF73D26C87DAE4073D7B6B7875AEC589305` |
| [runtime/execution_services.hpp](../../include/lasercnc/runtime/execution_services.hpp) | `1704301A95463C7EAC2A10F20ABC5ED97B7D3E94C6D50BCB144E6194B85D5D9A` |
| [runtime/history_runtime.hpp](../../include/lasercnc/runtime/history_runtime.hpp) | `CDB5913A438D72A107AE2F1CA7E34343B32E46B5E08A8A8095FFF6FB543FEC74` |
| [runtime/lifecycle_catalog.hpp](../../include/lasercnc/runtime/lifecycle_catalog.hpp) | `12D33AF10135EDE0273C215B1FB9595AB39F5F120C4004DCE513EF25E62EF2E5` |
| [runtime/project_runtime.hpp](../../include/lasercnc/runtime/project_runtime.hpp) | `7A15E2433AAEC8FC41D30AC42937EB8FF9C57B4385E2077D889348F73D298143` |
| [runtime/query_registry.hpp](../../include/lasercnc/runtime/query_registry.hpp) | `86FBA38A1CBE779CDA6267B9122D30B621B7F0CE143A67AEA2296BD84FD2E7F8` |
| [runtime/query_runtime.hpp](../../include/lasercnc/runtime/query_runtime.hpp) | `9EA2DE83F7F6F69C3B1F1CF1FD60769171B4ABFE0290844A21E8861FAF7BDB24` |
| [runtime/query.hpp](../../include/lasercnc/runtime/query.hpp) | `4A9341C06E74DB5F956BC0FA5950E06879FC3DA233E9D6EDE6D32E51E49BAA56` |
| [runtime/resource_manager.hpp](../../include/lasercnc/runtime/resource_manager.hpp) | `09B459D8A1077AEA30FA03DFC75B786E75CE5432A4EFD5E059DB6D79DA5E7783` |
| [runtime/scheduler.hpp](../../include/lasercnc/runtime/scheduler.hpp) | `2193AEBDAFD0700AEA7FDA12649265CE540BDF42AB63E1FC2CF4C87160691563` |
| [runtime/script_registry.hpp](../../include/lasercnc/runtime/script_registry.hpp) | `4316F0E8D6E8D0A290DF1FBB957B8269020AC27B71CAB05238D4FD7859971B89` |
| [runtime/script_runtime.hpp](../../include/lasercnc/runtime/script_runtime.hpp) | `515BB776253FAA16B20B06C8BCE40DB9E2FE8D1D5ABE950C465FFB3AA8282482` |
| [runtime/script.hpp](../../include/lasercnc/runtime/script.hpp) | `429DFE556A95B9021A715E41C9B2B8677AA2C65D49DEF580C4064463E504C4D2` |
| [runtime/task_registry.hpp](../../include/lasercnc/runtime/task_registry.hpp) | `351160A1563197243B6772624C9FA5A168CC5A90D10BFFC024146715FC3992FF` |
| [runtime/task_runtime.hpp](../../include/lasercnc/runtime/task_runtime.hpp) | `3171D508D87A3245D756574D3431DF3AC0CB1EC0C7740A4C85C87F4B66CC5B86` |
| [runtime/task.hpp](../../include/lasercnc/runtime/task.hpp) | `E004D16830EAC9467897338A974BC280D92FD4B60E1C8BC9C4B4F081F17F6BCC` |
| [runtime/transaction_manager.hpp](../../include/lasercnc/runtime/transaction_manager.hpp) | `F8E102F9A36E83408F66AB54EF80B55998BA06C347D4E561D1A7767CAA48779F` |
| [runtime/transaction.hpp](../../include/lasercnc/runtime/transaction.hpp) | `46DB8AA664433C503BF4B36389E4971B367B1906F7F265C44D92B6A9CD82C1E7` |
| [runtime/workflow_registry.hpp](../../include/lasercnc/runtime/workflow_registry.hpp) | `F06FA8A81E5F12E0F52D3F250277246C5C91A8ACDA9CCCE651FF57918FF7C07C` |
| [runtime/workflow_runtime.hpp](../../include/lasercnc/runtime/workflow_runtime.hpp) | `4C044BE5B108F937AEF9858B219F4A6B9A3E63194B5E561CE2F3CAC3C6313EF9` |
| [runtime/workflow.hpp](../../include/lasercnc/runtime/workflow.hpp) | `49A31A0C13801672AC4E591D7B588C1F20C6C39C8CF7450A3B5311E7CC18CEA2` |
| [state/asset_ref.hpp](../../include/lasercnc/state/asset_ref.hpp) | `3EA59EF06F9EA0E885AB158D90205096838D058DE058D06C39250B30619838AC` |
| [state/document_store.hpp](../../include/lasercnc/state/document_store.hpp) | `B9C1417B1D24D0C73F4B3C627446521CBB9152C851043F4D2888D552BFCD1ED2` |
| [state/document.hpp](../../include/lasercnc/state/document.hpp) | `A14540B33AD66626D9111937E8E12F4DA498930F6D0C98384A6ACC97864C7867` |
| [state/object_registry.hpp](../../include/lasercnc/state/object_registry.hpp) | `2B4F62030B1455AB57FE13EECFF543F418D73D22D284D74AF676ADD8F4FD136D` |
| [state/object_type_registry.hpp](../../include/lasercnc/state/object_type_registry.hpp) | `AE930B7589551C04738CC4C67F867552859F08B4093B9DB395E65B99AFDF58AD` |
| [state/revision.hpp](../../include/lasercnc/state/revision.hpp) | `7C363A1B7A1E6D233DC9860B688F5380BEAD5196733C8E038F8A7B618D09319B` |
