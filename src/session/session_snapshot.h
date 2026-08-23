#pragma once

namespace cha {

class Workspace;


// During workspace reload, refresh portable SQL snapshots for active local
// databases and bootstrap active databases that exist only as SQL. A database
// archived under deleted/ suppresses import of the matching snapshot.
void export_and_bootstrap_sessions(
    const Workspace& workspace);

} // namespace cha
