#pragma once

namespace cha {

class Workspace;


// During workspace reload, bootstrap active databases that exist only as SQL.
// A database archived under deleted/ suppresses import of the matching
// snapshot.
void bootstrap_sessions_from_sql(
    const Workspace& workspace);

} // namespace cha
