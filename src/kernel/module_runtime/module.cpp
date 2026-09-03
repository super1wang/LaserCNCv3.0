#include <lasercnc/kernel/module.hpp>
#include <lasercnc/kernel/module_registrar.hpp>

namespace lasercnc::kernel {

foundation::Result<void> IModule::registerComponents(ModuleRegistrar&)
{
    return foundation::Result<void>::success();
}

foundation::Result<void> IModule::initialize(AppKernel&)
{
    return foundation::Result<void>::success();
}

foundation::Result<void> IModule::start(AppKernel&)
{
    return foundation::Result<void>::success();
}

foundation::Result<void> IModule::stop(AppKernel&)
{
    return foundation::Result<void>::success();
}

} // namespace lasercnc::kernel
