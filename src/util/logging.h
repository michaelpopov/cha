#pragma once

namespace cha {

// Enables the named, file-only diagnostic logger when CHA_LOG_FILE is set.
// Call this once after load_dotenv() and before worker threads are started.
void initialize_diagnostic_logging();

} // namespace cha
