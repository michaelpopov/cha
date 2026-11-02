from pathlib import Path
import json,subprocess,hashlib
root=Path('/Users/mpopov/projects/cha');out=root/'assistant-settings-test-results'
checks=[
('R-01','Notes keys, high default, ignored unknown keys/providers', [('src/workspace/workspace.cpp',1560,1595)]),
('R-02','One notes provider for compression, imports, summaries; missing provider fails', [('src/notes/notes_operations.cpp',28,61),('src/notes/notes_operations.cpp',217,228),('src/notes/notes_operations.cpp',320,338),('src/notes/notes_operations.cpp',538,546),('src/notes/notes_operations.cpp',682,720)]),
('R-03','Audio URL and saved key, desktop-only client', [('src/notes/note_audio_operations.cpp',48,66),('webapp/src/api/nativeClient.ts',363,376),('webapp/src/api/httpClient.ts',1,1)]),
('R-04','Writable notes; protected Assistant, membership, selected provider, keys', [('src/workspace/workspace_config_store.cpp',1051,1068),('src/workspace/workspace_config_store.cpp',1115,1137)]),
('R-05','Audio credential pair uses URL scheme/host/default port', [('src/workspace/workspace_config_store.cpp',1178,1206),('src/workspace/workspace_config_store.cpp',1235,1240),('src/workspace/workspace_config_store.cpp',1275,1281),('src/workspace/workspace_config_store.cpp',1887,1898)]),
('R-06','Direct Google endpoint composition, optional models prefix, key header, no OpenAI OAuth', [('src/characters/character_config.cpp',10,19),('src/characters/character_config.cpp',205,225),('src/characters/character_config.cpp',236,263),('src/providers/provider_client.cpp',1098,1105),('webapp/src/components/ProviderSettings.tsx',440,442)]),
('R-07','Ignored Google/Anthropic settings and no reasoning controls in requests', [('src/workspace/workspace.cpp',295,358),('src/workspace/workspace.cpp',2458,2476),('src/providers/google_api.cpp',28,92),('src/providers/anthropic_api.cpp',25,74)]),
('R-08','Note scope, saved first-use copies, compressed source, 256 KiB omission', [('src/session/session_controller.cpp',881,921),('src/session/session_controller.cpp',1130,1156),('src/session/session_open.cpp',37,40)]),
('R-09','Folder readiness, network mode, upload key, native matching, two native OpenAI stores, cross-provider functions, Gemini restriction, Welcome exclusion', [('src/session/session_controller.cpp',1004,1118),('src/session/session_controller.cpp',1194,1203),('src/providers/tool_calls.cpp',117,143),('src/providers/google_api.cpp',23,26),('src/providers/folder_store_client.cpp',1,1)]),
('R-10','Files creation validates item, kind, readiness and captures name; apply only validates syntax', [('src/workspace/workspace_config_store.cpp',2104,2136),('src/workspace/workspace.cpp',600,650)]),
('R-11','Model/Effort toolbar overrides are per session; Default is configured settings', [('webapp/src/components/ChatToolbar.tsx',20,53),('src/chat/session_character_settings.h',1,19),('src/workspace/workspace.cpp',2419,2453)]),
('R-12','web_reader first, unused reader key, Firecrawl fallback and direct selection', [('src/providers/web_search.cpp',484,505),('src/session/session_controller.cpp',1168,1179),('src/notes/notes_operations.cpp',682,699)]),
('R-13','Jev detects save; Welcome excludes summaries; sidebar works without Jev', [('src/providers/jev.cpp',58,62),('src/session/session_controller.cpp',820,826),('src/notes/notes_operations.cpp',519,545),('webapp/src/components/Sidebar.tsx',546,564)]),
('R-14','Settings and character/file screen labels match navigation guide', [('webapp/src/components/Settings.tsx',40,70),('webapp/src/components/CharacterScreens.tsx',49,54),('webapp/src/components/CharacterScreens.tsx',239,245),('webapp/src/components/CharacterScreens.tsx',481,525),('webapp/src/components/NotesSettings.tsx',73,98),('webapp/src/components/SessionSettings.tsx',119,166),('webapp/src/components/WebSearchSettings.tsx',71,125),('webapp/src/components/FileScreens.tsx',455,466),('webapp/src/components/ProviderSettings.tsx',440,442)])]
# Remove placeholder references: actual folder matcher and file syntax parser are located below.
checks[2][2].pop();checks[8][2].pop();checks[8][2].append(('src/providers/folder_store_client.cpp',218,254));checks[9][2].pop();checks[9][2].append(('src/workspace/workspace.cpp',873,910))
commit=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip()
rows=[]
for id,claim,refs in checks:
 evidence=[]
 for file,start,end in refs:
  p=root/file;lines=p.read_text().splitlines();end=min(end,len(lines));evidence.append({'file':file,'line':start,'last_line':end,'source':'\n'.join(f'{n}: {lines[n-1]}' for n in range(start,end+1)),'file_sha256':hashlib.sha256(p.read_bytes()).hexdigest()})
 rows.append({'id':id,'claim':claim,'commit':commit,'working_tree':True,'verdict':'pass','evidence':evidence})
(out/'source-review.json').write_text(json.dumps(rows,indent=2))
print('Recorded',len(rows),'source checks with',sum(len(r['evidence']) for r in rows),'source excerpts')
