#include "app/application.h"
#include "support/test_workspace.h"
#include "workspace/workspace_config_store.h"
#include "workspace/builtins.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
using namespace cha; using Json=nlohmann::json;
const std::filesystem::path root=std::filesystem::path(std::getenv("HOME"))/"Library/Application Support/CHA";
const std::filesystem::path db=root/"Assistant-Capability-Test-20261101.sqlite3";
const std::filesystem::path run="/private/tmp/cha-ass-settings-run";
Json load(){Json j;std::ifstream(run/"fixture.json")>>j;return j;}
int main(int argc,char**argv){try{
 std::string action=argc>1?argv[1]:"";
 if(action=="setup"){
  if(std::filesystem::exists(db))throw std::runtime_error("Fixture already exists");
  test::TestWorkspace seed; (void)test::import_test_database(seed.root(),db);
  auto s=WorkspaceConfigStore::open(db); Json j;
  const auto key=[&](const char* id,const char* label,const char* env){const char*v=std::getenv(env);if(!v||!*v)throw std::runtime_error(std::string("Missing ")+env);s->apply_api_key_create(id,label,v);};
  key("api_key_1","OpenAI","OPENAI_API_KEY");key("api_key_2","Gemini","GEMINI_API_KEY");
  s->apply_api_key_create("api_key_3","Audio","[TEST CREDENTIAL OMITTED]");
  // No Anthropic key is available. Keep a saved placeholder for knowledge checks only.
  s->apply_api_key_create("api_key_4","Anthropic unavailable","[TEST CREDENTIAL OMITTED]");
  auto provider=[&](std::string label,ProviderApi api,std::string host,std::string model,std::string path,std::string key){auto id=s->create_provider(label);ModelBackendConfig c{.host=host,.port=443,.base_path=path,.mode=Mode::net,.model=model,.stream=true,.timeout_s=120,.idle_timeout_s=60,.api_key_id=key,.reasoning_effort="none",.https=true,.api=api};s->apply_provider_update(id,label,c);return id;};
  j["openai"]=provider("openai-direct",ProviderApi::responses,"api.openai.com","gpt-4o","","api_key_1");
  j["google"]=provider("google-direct",ProviderApi::google,"generativelanguage.googleapis.com","gemini-3.8-flash","/v1beta","api_key_2");
  j["anthropic"]=provider("anthropic-direct",ProviderApi::anthropic,"api.anthropic.com","claude-sonnet-4-5","","api_key_4");
  (void)s->apply_character_settings(workspace_assistant_id,j["openai"].get<std::string>(),{});
  for(auto [name,p]:std::vector<std::pair<std::string,std::string>>{{"Writer",j["openai"]},{"Gem",j["google"]},{"Claude Test",j["anthropic"]}}){auto id=s->create_character(name,"Test character");(void)s->apply_character_settings(id,p,{}, {},name=="Claude Test"?std::optional<std::string_view>("high"):std::nullopt);j[name]=id;}
  auto forum=s->create_forum("Lab",workspace_guest_id);j["lab"]=forum;
  (void)s->apply_forum_members_and_persona(forum,std::vector<std::string>{j["Writer"],j["Gem"],j["Claude Test"],std::string(workspace_assistant_id)},workspace_guest_id);
  for(auto [name,size]:std::vector<std::pair<std::string,int>>{{"Long note",4300},{"Huge note",270000}}){auto n=s->create_note_item({},NoteItemKind::note,name);(void)s->save_note(n.id,Json{{"type","doc"},{"content",Json::array({{{"type","paragraph"},{"content",Json::array({{{"type","text"},{"text",std::string(size,'a')}}})}}})}});(void)s->set_note_read_only(n.id,true);j[name]={{"id",n.id},{"ref_id",note_item_ref_id(n.id)}};(void)s->create_file_reference(FileReferenceOwner::forum,forum,FileReferenceKind::note,note_item_ref_id(n.id));}
  for(auto name:{"Docs OpenAI","Docs Gemini","Docs Removed"}){auto n=s->create_note_item({},NoteItemKind::folder,name);auto note=s->create_note_item(n.id,NoteItemKind::note,"Fixture document");(void)s->save_note(note.id,Json{{"type","doc"},{"content",Json::array({{{"type","paragraph"},{"content",Json::array({{{"type","text"},{"text","Test fixture content for Assistant capability checks."}}})}}})}});(void)s->set_note_read_only(note.id,true);j[name]={{"id",n.id},{"ref_id",note_item_ref_id(n.id)}};}
  std::ofstream(run/"fixture.json")<<j.dump(2);std::ofstream(root/"ass-test-vault.toml")<<"vault_name = 'Assistant-Capability-Test-20261101'\ndata = 'Assistant-Capability-Test-20261101.sqlite3'\n";
  std::cout<<j.dump()<<std::endl;return 0;
 }
 if(action=="export"){std::filesystem::path dest=argv[2];std::filesystem::create_directories(dest);auto result=export_workspace_configuration(db,dest,WorkspaceConfigLease::already_held);std::cout<<result.file_count<<std::endl;return 0;}
 auto j=load();auto s=WorkspaceConfigStore::open(db);
 if(action=="inventory"){Json a=Json::array();for(auto& x:s->list_folder_stores())a.push_back({{"id",x.id},{"folder_id",x.folder_id},{"service",folder_store_service_name(x.service)},{"key_id",x.api_key_id},{"remote_id",x.remote_store_id?Json(*x.remote_store_id):Json(nullptr)},{"files",x.file_ids}});std::cout<<a.dump(2)<<std::endl;}
 else if(action=="fix-paths"){for(auto name:{"openai","anthropic"}){auto p=s->snapshot()->find_provider(j[name].get<std::string>());auto c=p->config;c.base_path="";s->apply_provider_update(j[name].get<std::string>(),std::string(name)+"-direct",c);}}
 else if(action=="notes") {WorkspaceNotes n{.compression_provider_id=j[argc>2?argv[2]:"openai"],.compression_reasoning_effort=argc>3?argv[3]:"low"};if(argc>4){n.audio_url="https://tts.example.com";n.audio_api_key_id="api_key_3";}s->apply_notes_update(n);}
 else if(action=="google-break"||action=="google-fix"){auto p=s->snapshot()->find_provider(j["google"].get<std::string>());auto c=p->config;c.base_path=action=="google-break"?"":"/v1beta";s->apply_provider_update(j["google"].get<std::string>(),"google-direct",c);}
 else throw std::runtime_error("Unknown action");
}catch(const std::exception&e){std::cerr<<e.what()<<std::endl;return 1;}}
