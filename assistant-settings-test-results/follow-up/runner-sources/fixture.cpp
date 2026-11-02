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
const std::filesystem::path run="/private/tmp/cha-ass-followup";
const std::filesystem::path db=root/"Assistant-Capability-Followup.sqlite3";
Json load(){Json j;std::ifstream(run/"fixture.json")>>j;return j;}
ModelBackendConfig config(std::string host,std::string model,ProviderApi api,std::string key,std::string path=""){
 return {.host=host,.port=443,.base_path=path,.mode=Mode::net,.model=model,.stream=true,.timeout_s=180,.idle_timeout_s=90,.api_key_id=key,.reasoning_effort="none",.https=true,.api=api};
}
int main(int argc,char**argv){try{
 std::string action=argc>1?argv[1]:"";
 if(action=="setup"){
  if(std::filesystem::exists(db))throw std::runtime_error("Fixture already exists");
  test::TestWorkspace seed;(void)test::import_test_database(seed.root(),db);
  auto s=WorkspaceConfigStore::open(db);Json j;
  const auto key=[&](const char*id,const char*label,const char*env){const char*v=std::getenv(env);if(!v||!*v)throw std::runtime_error(std::string("Missing ")+env);s->apply_api_key_create(id,label,v);};
  key("api_key_1","OpenAI","OPENAI_API_KEY");key("api_key_2","Gemini","GEMINI_API_KEY");
  s->apply_api_key_create("api_key_3","Audio","[TEST CREDENTIAL OMITTED]");
  if(const char*v=std::getenv("ANTHROPIC_API_KEY");v&&*v){key("api_key_4","Anthropic","ANTHROPIC_API_KEY");j["anthropic_credential"]=true;}
  else{s->apply_api_key_create("api_key_4","Anthropic unavailable","[TEST CREDENTIAL OMITTED]");j["anthropic_credential"]=false;}
  s->apply_api_key_create("api_key_5","Firecrawl","[TEST CREDENTIAL OMITTED]");
  const auto add=[&](const std::string& label,const ModelBackendConfig& c){auto id=s->create_provider(label);s->apply_provider_update(id,label,c);return id;};
  auto assistant=config("api.openai.com","gpt-6.1-sol",ProviderApi::responses,"api_key_1");assistant.reasoning_effort="low";
  j["assistant"]=add("Assistant test model",assistant);
  j["openai"]=add("openai-direct",config("api.openai.com","gpt-5.4-mini",ProviderApi::responses,"api_key_1"));
  j["google"]=add("google-direct",config("generativelanguage.googleapis.com","gemini-3.8-flash",ProviderApi::google,"api_key_2","/v1beta"));
  j["anthropic"]=add("anthropic-direct",config("api.anthropic.com","claude-sonnet-4-5",ProviderApi::anthropic,"api_key_4"));
  (void)s->apply_character_settings(workspace_assistant_id,j["assistant"].get<std::string>(),{});
  s->apply_session_naming_update({.provider_id="test"});
  for(auto [name,p]:std::vector<std::pair<std::string,std::string>>{{"Writer",j["openai"]},{"Gem",j["google"]},{"Claude Test",j["anthropic"]}}){auto id=s->create_character(name,"Test character");(void)s->apply_character_settings(id,p,{}, {},name=="Claude Test"?std::optional<std::string_view>("high"):std::nullopt);j[name]=id;}
  auto forum=s->create_forum("Lab",workspace_guest_id);j["lab"]=forum;
  (void)s->apply_forum_members_and_persona(forum,std::vector<std::string>{j["Writer"],j["Gem"],j["Claude Test"],std::string(workspace_assistant_id)},workspace_guest_id);
  for(auto [name,size]:std::vector<std::pair<std::string,int>>{{"Long note",4300},{"Huge note",270000}}){auto n=s->create_note_item({},NoteItemKind::note,name);(void)s->save_note(n.id,Json{{"type","doc"},{"content",Json::array({{{"type","paragraph"},{"content",Json::array({{{"type","text"},{"text",std::string(size,'a')}}})}}})}});(void)s->set_note_read_only(n.id,true);j[name]={{"id",n.id},{"ref_id",note_item_ref_id(n.id)}};(void)s->create_file_reference(FileReferenceOwner::forum,forum,FileReferenceKind::note,note_item_ref_id(n.id));}
  for(auto name:{"Docs OpenAI","Docs Gemini","Docs Removed"}){auto n=s->create_note_item({},NoteItemKind::folder,name);auto note=s->create_note_item(n.id,NoteItemKind::note,"Fixture document");(void)s->save_note(note.id,Json{{"type","doc"},{"content",Json::array({{{"type","paragraph"},{"content",Json::array({{{"type","text"},{"text","Test fixture content for Assistant capability checks."}}})}}})}});(void)s->set_note_read_only(note.id,true);j[name]={{"id",n.id},{"ref_id",note_item_ref_id(n.id)}};}
  std::ofstream(run/"fixture.json")<<j.dump(2);std::ofstream(root/"ass-followup-vault.toml")<<"vault_name = 'Assistant-Capability-Followup'\ndata = 'Assistant-Capability-Followup.sqlite3'\n";
  std::cout<<j.dump()<<std::endl;return 0;
 }
 if(action=="reset"){(void)import_workspace_configuration(run/"baseline-private",db);return 0;}
 if(action=="export"){std::filesystem::path dest=argv[2];std::filesystem::create_directories(dest);auto result=export_workspace_configuration(db,dest,WorkspaceConfigLease::already_held);std::cout<<result.file_count<<std::endl;return 0;}
 auto j=load();auto s=WorkspaceConfigStore::open(db);
 if(action=="inventory"){Json a=Json::array();for(auto&x:s->list_folder_stores())a.push_back({{"id",x.id},{"folder_id",x.folder_id},{"service",folder_store_service_name(x.service)},{"key_id",x.api_key_id},{"remote_id",x.remote_store_id?Json(*x.remote_store_id):Json(nullptr)},{"files",x.file_ids}});std::cout<<a.dump(2)<<std::endl;}
 else if(action=="notes"){WorkspaceNotes n{.compression_provider_id=j[argc>2?argv[2]:"openai"],.compression_reasoning_effort=argc>3?argv[3]:"low"};if(argc>4){n.audio_url="https://tts.example.com";n.audio_api_key_id="api_key_3";}s->apply_notes_update(n);}
 else if(action=="model"){auto p=s->snapshot()->find_provider(j["assistant"].get<std::string>());auto c=p->config;c.model=argv[2];c.reasoning_effort=c.model=="gpt-4o"?"none":"low";s->apply_provider_update(j["assistant"].get<std::string>(),"Assistant test model",c);}
 else if(action=="google-break"||action=="google-fix"){auto p=s->snapshot()->find_provider(j["google"].get<std::string>());auto c=p->config;c.base_path=action=="google-break"?"":"/v1beta";s->apply_provider_update(j["google"].get<std::string>(),"google-direct",c);}
 else if(action=="restore-assistant"){(void)s->apply_forum_members_and_persona(j["lab"].get<std::string>(),std::vector<std::string>{j["Writer"],j["Gem"],j["Claude Test"],std::string(workspace_assistant_id)},workspace_guest_id);(void)s->apply_forum_default_character(j["lab"].get<std::string>(),workspace_assistant_id);}
 else if(action=="gem-reset"){(void)s->apply_character_settings(j["Gem"].get<std::string>(),j["google"].get<std::string>(),{});}
 else if(action=="reader"){s->apply_web_search_update({.read_provider="firecrawl",.firecrawl_api_key_id="api_key_5",.web_reader_url="http://127.0.0.1:9",.web_reader_enabled=true,.web_reader_api_key_id="api_key_3"});}
 else if(action=="reader-off"){s->apply_web_search_update({});}
 else throw std::runtime_error("Unknown action");
}catch(const std::exception&e){std::cerr<<e.what()<<std::endl;return 1;}}
