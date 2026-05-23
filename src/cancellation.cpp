#include "cancellation.h"

namespace praia { namespace detail {

thread_local CancellationState* g_currentCancel = nullptr;

}}  // namespace praia::detail
