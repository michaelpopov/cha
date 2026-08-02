#include "ui/web/asset_handler.h"

#include <httplib.h>

namespace cha::web {

void AssetHandler::install(httplib::Server& server) const {
    server.Get("/", [](const httplib::Request&, httplib::Response& response) {
        response.set_content(
            R"html(<!doctype html><html lang="en"><head><meta charset="utf-8">
<title>cha</title></head><body><main><h1>cha</h1>
<p id="status">Loading personas…</p>
<section id="persona-screen"><h2>Choose a persona</h2><ul id="personas"></ul></section>
<section id="forum-screen" hidden><h2>Choose a forum</h2><p id="chosen-persona"></p><ul id="forums"></ul></section>
<section id="session-screen" hidden><h2>Choose a session</h2><p id="chosen-forum"></p><ul id="sessions"></ul><button id="new-session" type="button">New session</button></section>
<script>
(() => {
  let forum;
  const status = document.getElementById('status');
  const request = async path => {
    const response = await fetch(path);
    if (!response.ok) throw new Error('Request failed');
    return response.json();
  };
  const showError = error => { status.textContent = error.message; };
  const button = (text, action) => {
    const element = document.createElement('button');
    element.type = 'button';
    element.textContent = text;
    element.addEventListener('click', action);
    return element;
  };
  const openSession = async id => {
    try {
      const response = await fetch('/api/v1/forums/' + encodeURIComponent(forum.id) + '/sessions/' + encodeURIComponent(id) + '/open', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: '{}'});
      if (!response.ok) throw new Error('Could not open session');
      location.assign((await response.json()).path);
    } catch (error) { showError(error); }
  };
  const chooseForum = async selected => {
    forum = selected;
    document.getElementById('chosen-forum').textContent = selected.display_name;
    const sessions = document.getElementById('sessions');
    sessions.replaceChildren();
    try {
      for (const entry of await request('/api/v1/forums/' + encodeURIComponent(selected.id) + '/sessions')) {
        const item = document.createElement('li');
        item.append(button(entry.label, () => openSession(entry.id)));
        sessions.append(item);
      }
      document.getElementById('session-screen').hidden = false;
      status.textContent = '';
    } catch (error) { showError(error); }
  };
  document.getElementById('new-session').addEventListener('click', async () => {
    const label = prompt('Session name');
    if (label === null) return;
    try {
      const response = await fetch('/api/v1/forums/' + encodeURIComponent(forum.id) + '/sessions', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({label})});
      if (!response.ok) throw new Error('Could not create session');
      openSession((await response.json()).id);
    } catch (error) { showError(error); }
  });
  (async () => {
    try {
      const personas = document.getElementById('personas');
      for (const entry of await request('/api/v1/personas')) {
        const item = document.createElement('li');
        item.append(button(entry.display_name, async () => {
          sessionStorage.setItem('cha.persona', entry.id);
          forum = undefined;
          document.getElementById('session-screen').hidden = true;
          document.getElementById('chosen-persona').textContent = entry.display_name;
          document.getElementById('forum-screen').hidden = false;
          const forums = document.getElementById('forums');
          forums.replaceChildren();
          try {
            for (const candidate of await request('/api/v1/forums')) {
              const forumItem = document.createElement('li');
              forumItem.append(button(candidate.display_name, () => chooseForum(candidate)));
              forums.append(forumItem);
            }
            status.textContent = '';
          } catch (error) { showError(error); }
        }));
        personas.append(item);
      }
      status.textContent = '';
    } catch (error) { showError(error); }
  })();
})();
</script></main></body></html>)html",
            "text/html; charset=utf-8");
    });
}

void AssetHandler::set_chat_page(httplib::Response& response) {
    response.set_content(
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<title>cha session</title></head><body><main><h1>cha</h1>"
        "<p>The chat browser is not installed yet.</p></main></body></html>",
        "text/html; charset=utf-8");
}

void AssetHandler::set_session_not_open_page(httplib::Response& response) {
    response.set_content(
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<title>Session not open</title></head><body><main>"
        "<h1>Session is not open</h1><p><a href=\"/\">Return to the lobby</a>"
        "</p></main></body></html>",
        "text/html; charset=utf-8");
}

} // namespace cha::web
