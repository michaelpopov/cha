await bridge();await rpc('session.open',C.welcome);await submit(C.welcome,'/clear');
await checkpoint('W01-before');await ask('answer','What do my Notes settings do?');await checkpoint('W01-after');