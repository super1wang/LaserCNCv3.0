#include <lasercnc/kernel/module.hpp>

namespace lasercnc::kernel {

foundation::Result<void> IModule::registerServices(ServiceRegistry&)
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
