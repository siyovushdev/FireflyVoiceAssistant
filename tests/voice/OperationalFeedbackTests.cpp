#include "Support.h"
#include "firefly/dialogue/OperationalDialogue.h"
#include <iostream>
using namespace firefly;
using admin_test::Json;
int main(int argc,char**argv) {
 try {
  if(argc!=2)return 2;
  std::ifstream in(std::string(argv[1])+"/tests/admin/v2/system.json");auto doc=Json::parse(in);
  doc["devices"]=Json::array({doc["devices"][0]});doc["devices"][0]["id"]="lamp";
  doc["devices"][0]["name"]="Свет";doc["devices"][0]["spoken_aliases"]={"свет"};
  auto catalog=admin_test::catalog(doc);std::int64_t now=1000;
  execution::DeviceStateStore store(catalog,[&]{return now;},[]{return 100000;});
  const actions::GenericAction on("action_on","lamp",{});
  auto e=execution::MockDeviceExecutor(catalog).preview(on);e.status=execution::ExecutionStatus::Accepted;
  int checks=0;auto check=[&](bool v,const std::string& message){++checks;if(!v)throw std::runtime_error(message);};
  auto facts=[&]{return dialogue::Facts{catalog,1,now,store.devices(),{}};};
  auto confirmed=[&](dialogue::Facts f){return dialogue::OperationalDialogue::executionConfirmed(f,e,on,1000);};
  dialogue::OperationalDialogue d;
  check(!confirmed(facts()),"seed never confirms");
  store.applyObservation({"lamp",{{"power",actions::Scalar(true)}}});now=1001;
  check(!dialogue::OperationalDialogue::executionConfirmed(facts(),e,on,1001),"old fresh observation cannot silence a new command");
  store.applyPrediction("lamp",{{"power",actions::Scalar(true)}},100000);
  check(!confirmed(facts()),"prediction/PUBACK not confirmation");
  check(d.commandReply(facts(),e,{},false)=="Команду отправила, ответа пока нет.","accepted unknown is concise and truthful");
  store.applyObservation({"lamp",{{"power",actions::Scalar(false)}}});
  check(!confirmed(facts()),"opposite fresh sample does not confirm");
  check(d.commandReply(facts(),e,{},false,1000,&on)=="Устройство ответило, но выполнение не подтвердилось.","wrong-state response is not called missing");
  store.applyObservation({"lamp",{{"power",actions::Scalar(true)}}});check(confirmed(facts()),"independent matching post-command observation confirms");
  check(d.commandReply(facts(),e,{},false,1000,&on).empty()&&d.commandReply(facts(),e,{},true,1000,&on).empty(),"confirmed commands silent in both modes");
  auto f=facts();f.failedReads={"lamp"};check(!confirmed(f),"failed explicit read invalidates cached confirmation for this reply");
  f=facts();f.devices[0].properties["power"].orderSteadyMs=now+1;check(!confirmed(f),"future order rejected");
  f=facts();f.devices[0].properties["power"].freshnessSteadyMs.reset();check(!confirmed(f),"missing local timestamp rejected");
  f=facts();f.devices[0].properties["power"].freshnessSteadyMs=now+1;check(!confirmed(f),"future arrival rejected");
  auto malformed=e;malformed.current=execution::LightState{100};
  check(!dialogue::OperationalDialogue::executionConfirmed(facts(),malformed,on,1000),"wrong typed state variant rejected");
  auto failed=e;failed.success=false;failed.status=execution::ExecutionStatus::Failed;failed.error="mqtt_publish_timeout";
  check(!dialogue::OperationalDialogue::executionConfirmed(facts(),failed,on,1000),"failed delivery never silent success even with ON state");
  check(d.commandReply(facts(),failed,{},false)=="Не удалось подтвердить выполнение.","timeout does not deny possible delivery");
  now+=60001;check(!confirmed(facts()),"stale matching sample rejected");now=2000;
  store.applyObservation({"lamp",{{"power",actions::Scalar(true)}}});
  auto q=d.ask("Свет включен?",facts(),true,false);check(q.text=="Да.","confirmed ON direct answer");
  q=d.ask("Свет выключен?",facts(),true,false);check(q.text=="Нет.","OFF predicate is inverted correctly");
  store.applyObservation({"lamp",{{"power",actions::Scalar(false)}}});
  check(d.ask("Свет включен?",facts(),true,false).text=="Нет.","confirmed OFF direct answer");
  check(d.ask("Свет выключен?",facts(),true,false).text=="Да.","confirmed OFF positive predicate");
  check(d.ask("Свет не включен?",facts(),true,false).text=="Питание выключено.","negative question reports state rather than ambiguous yes/no");
  now+=60001;check(d.ask("Свет включен?",facts(),true,false).text=="Сейчас не могу проверить.","stale not spoken as no");
  now=3000;int reads=0;
  auto refresh=[&](const std::vector<std::string>& ids){++reads;check(ids==std::vector<std::string>{"lamp"},"refresh receives only exact scoped target");auto x=facts();x.failedReads=ids;return x;};
  d.ask("Свет включен?",facts(),true,false,{},refresh);check(reads==1,"valid state question reads");
  for(const auto* text:{"Включи свет","Не выключай свет","Чайник включен?","Свет на Марсе включен?"})d.ask(text,facts(),true,false,[](auto){return dialogue::Question::State;},refresh);
  d.ask("Свет включен?",facts(),false,false,{},refresh);check(reads==1,"commands, unknown scope, malicious Qwen and disabled questions never read");
  auto mixed=doc;auto other=mixed["devices"][0]["state"][0];other["id"]="aux";other["name"]="Дополнительный выход";mixed["devices"][0]["state"].push_back(other);
  mixed["actions"][0]["assignments"].push_back({{"field","aux"},{"value",false}});
  auto multi=admin_test::catalog(mixed);execution::DeviceStateStore all(multi,[&]{return now;});
  auto multiReceipt=execution::MockDeviceExecutor(multi).preview(on);multiReceipt.status=execution::ExecutionStatus::Accepted;
  all.applyObservation({"lamp",{{"power",actions::Scalar(true)}}});
  auto allFacts=[&]{return dialogue::Facts{multi,1,now,all.devices(),{}};};
  check(!dialogue::OperationalDialogue::executionConfirmed(allFacts(),multiReceipt,on,now),"partial assignment evidence insufficient");
  all.applyObservation({"lamp",{{"aux",actions::Scalar(false)}}});
  check(dialogue::OperationalDialogue::executionConfirmed(allFacts(),multiReceipt,on,now),"all requested assignments confirmed");
  check(!dialogue::OperationalDialogue::executionConfirmed(allFacts(),multiReceipt,actions::GenericAction("action_off","lamp",{}),now),"different action never confirms");
  std::cout<<"PASS "<<checks<<" concise feedback/observation/provenance checks\n";
 }catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
