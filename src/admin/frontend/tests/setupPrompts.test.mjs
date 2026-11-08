import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { promptLimits, measurePrompt, planningLabel, containsPrivateCredentials } from '../.test-build/adapters/setupPrompt.js';
import { clone, powerOnly } from '../.test-build/adapters/editing.js';
import * as setup from '../.test-build/stores/setup.js';
import { setupAuth } from '../.test-build/api/setup.js';
import { manual } from '../.test-build/stores/manual.js';
const base = JSON.parse(readFileSync(new URL('../../../../config/empty-home.json', import.meta.url)));
const limits = { message_codepoints: 12000, message_bytes: 48000 };
const credentialCases = JSON.parse(readFileSync(new URL('../../../../tests/admin/setup-credential-cases.json', import.meta.url)));
const defer = () => { let resolve; const promise = new Promise(r => { resolve = r; }); return { promise, resolve }; };
function candidate() {
    const c = clone(base), d = { id:'lamp', name:'Лампа', kind:'generic', room_id:null, enabled:true, spoken_aliases:['лампа'], state:[], action_ids:[] };
    c.devices.push(d); powerOnly(c,d); return c;
}
async function fixture(handler) {
    const previous = globalThis.fetch, memory = new Map(), calls=[];
    globalThis.sessionStorage = { getItem:k=>memory.get(k)??null, setItem:(k,v)=>memory.set(k,v), removeItem:k=>memory.delete(k) };
    setupAuth.token=''; setup.session.value=null; setup.proposal.value=null; setup.setupBusy.value=false; setup.messagePending.value=false; setup.cancelBusy.value=false; setup.cancelRequested.value=false; setup.composer.value=''; setup.conversation.value=[]; setup.sendFeedback.value=''; setup.sendFailed.value=false; setup.context();
    manual.base.value=null; manual.candidate.value=null; manual.task.value=null; manual.busy.value=false;
    let state={ok:true,active:false,status:'idle',limits};
    const ctx={ calls, memory, get state(){return state;}, set state(v){state=v;}, close(){globalThis.fetch=previous;setupAuth.token='';setup.session.value=null;setup.proposal.value=null;setup.cancelRequested.value=false;} };
    globalThis.fetch=async(path, options={})=>{
        const body=options.body?JSON.parse(options.body):undefined; calls.push({path,body}); let reply;
        if(path==='/api/setup/status'||path==='/api/setup/heartbeat')reply=state;
        else if(path==='/api/config')reply={ok:true,hash:'base',revision:1,configuration:base};
        else if(path==='/api/setup/start')reply=state={ok:true,active:true,status:'editing',limits,version:0,session_id:'owner',configuration:base};
        else if(path==='/api/config/validate')reply={ok:true,valid:true};
        else if(path==='/api/setup/draft')reply=state={...state,version:state.version+1,configuration:body.configuration};
        else reply=await handler(path,body,ctx);
        return new Response(JSON.stringify(reply),{status:reply.ok===false?409:200});
    };
    await setup.startSetup(); return ctx;
}
test('prompt limits count Unicode codepoints and UTF-8 bytes without truncation; server capability has legacy fallback',()=>{
    assert.deepEqual(promptLimits(),{message_codepoints:4000,message_bytes:16000});
    assert.deepEqual(promptLimits({message_codepoints:NaN,message_bytes:-1}),promptLimits());
    const text='А😀\n';assert.deepEqual(measurePrompt(text,limits),{codepoints:3,bytes:7,exceeds:false});
    assert.equal(measurePrompt('я'.repeat(12000),limits).exceeds,false);
    assert.equal(measurePrompt('я'.repeat(12001),limits).exceeds,true);
    assert.equal(measurePrompt('😀'.repeat(100),{message_codepoints:100,message_bytes:399}).exceeds,true);
    assert.equal(planningLabel({phase:'reading',step:1,total_steps:3}),'Разбираем описание: шаг 1 из 3…');
    assert.equal(planningLabel({phase:'language_profile'}),'Подбираем речевые фразы…');
});
test('long mixed natural-language prompt reaches AI intact instead of HTTP extractor and last source survives reload',async()=>{
    const text='Создай кухню и подключённую MQTT лампу.\nHTTP адрес для другого прибора: http://192.168.1.8.\n'+ 'Пока нужны только включение и выключение, без яркости.\n'.repeat(90);
    const ctx=await fixture(async(path,body,c)=>{assert.equal(path,'/api/setup/message');assert.equal(body.text,text);return c.state={...c.state,version:1,configuration:candidate()};});
    try {
        setup.composer.value=text;await setup.sendMessage();assert.deepEqual(setup.setupIssues.value,[]);assert.ok(setup.proposal.value);assert.equal(setup.lane.value,'semantic');
        assert.equal(ctx.calls.filter(c=>c.path==='/api/setup/message').length,1);assert.equal(ctx.calls.some(c=>/apply|command|probe|poll/.test(c.path)),false);
        assert.equal(manual.candidate.value.devices.length,0,'not accepted or applied');
        setup.composer.value='';setup.conversation.value=[];await setup.restoreSession();assert.equal(setup.composer.value,text);assert.equal(setup.conversation.value[0].text,text);
        const saved=JSON.parse(ctx.memory.get('firefly-v2-setup-owner'));assert.equal(saved.sourceText,text);assert.equal(saved.conversation,undefined,'no persistent transcript');
    } finally {ctx.close();}
});
test('oversized text stays editable without HTTP message, incomplete checkpoint, or automatic retry',async()=>{
    const ctx=await fixture(()=>{throw Error('Unexpected mutation');});
    try {const text='я'.repeat(12001);setup.composer.value=text;await setup.sendMessage();assert.equal(setup.composer.value,text);assert.match(setup.setupIssues.value[0].message,/слишком длинный/);assert.equal(ctx.state.status,'editing');assert.equal(ctx.calls.filter(c=>c.path==='/api/setup/message').length,0);assert.equal(setup.ready.value,true);}finally{ctx.close();}
});
test('discard incomplete request restores checkpoint and source for editing, without resending or Apply',async()=>{
    const text='Создай лампу';const ctx=await fixture(async(path,body,c)=>{
        if(path==='/api/setup/message'){c.state={...c.state,version:1,status:'planning_failed',message_incomplete:true};return{ok:false,errors:[{code:'setup_failed',path:'',message:'invalid_ai_response'}]};}
        assert.equal(path,'/api/setup/discard_message');assert.equal(body.version,1);return c.state={...c.state,version:2,status:'editing',message_incomplete:false};
    });
    try {setup.composer.value=text;await setup.sendMessage();assert.equal(setup.ready.value,false);setup.composer.value='';await setup.discardMessage();assert.equal(setup.composer.value,text);assert.equal(setup.ready.value,true);assert.equal(ctx.calls.filter(c=>c.path==='/api/setup/message').length,1);assert.equal(ctx.calls.some(c=>/apply|command|probe|poll/.test(c.path)),false);}finally{ctx.close();}
});
test('status and cancellation reach server during a pending AI request; late result cannot become accepted proposal',async()=>{
    const entered=defer(),finish=defer();const ctx=await fixture(async(path,body,c)=>{
        if(path==='/api/setup/message'){c.state={...c.state,status:'planning',message_incomplete:true,progress:{phase:'reading',step:1,total_steps:2}};entered.resolve();await finish.promise;return {...c.state,status:'editing',version:1,configuration:candidate(),message_incomplete:false};}
        assert.equal(path,'/api/setup/cancel');assert.equal(body.session_id,'owner');return {ok:true,active:true,status:'cancel_pending'};
    });
    try {
        setup.composer.value='Создай лампу';const pending=setup.sendMessage();await entered.promise;
        await setup.pollStatus();assert.equal(setup.session.value.progress.phase,'reading');assert.equal(setup.canCancel.value,true);
        await setup.cancelSetup();assert.equal(setup.cancelRequested.value,true);assert.equal(setup.ready.value,false);
        ctx.state={...ctx.state,active:false,status:'cancelled_draft_saved'};finish.resolve();await pending;
        assert.equal(setup.proposal.value,null);assert.equal(manual.candidate.value.devices.length,0);assert.equal(ctx.calls.filter(c=>c.path==='/api/setup/cancel').length,1);
        assert.equal(ctx.calls.some(c=>/apply|command|probe|poll/.test(c.path)),false);
    } finally{finish.resolve();ctx.close();}
});
test('server-grounded connection proposal is validated before Review; unmarked physical changes retain legacy rejection',async()=>{
    for(const scoped of [false,true]) {
        const after=candidate();after.connections=[{id:'broker',name:'Брокер',type:'mqtt',enabled:true,timeout_ms:2000,network:{host:'127.0.0.1',port:1883},mqtt:{tls:false,credentials_id:'iot'}}];
        const ctx=await fixture(async(path,body,c)=>{assert.equal(path,'/api/setup/message');return c.state={...c.state,version:1,configuration:after,...(scoped?{proposal_scope:'configuration'}:{})};});
        try {setup.composer.value='Подключи лампу к MQTT';await setup.sendMessage();assert.equal(!!setup.proposal.value,scoped);assert.equal(ctx.calls.filter(c=>c.path==='/api/config/validate').length,scoped?1:0);assert.equal(ctx.calls.some(c=>/apply|command|probe|poll/.test(c.path)),false);assert.equal(manual.candidate.value.connections?.length??0,0);}finally{ctx.close();}
    }
});
test('private credentials stay only in editable composer and never reach session storage, history, or AI',async()=>{
    for(const text of ['MQTT password=not-a-real-secret','Логин=fictional','Пароль неизвестен', '{"token":"fictional"}']) {
        const ctx=await fixture(()=>{throw Error('Unexpected mutation');});
        try {setup.composer.value=text;await setup.sendMessage();assert.equal(setup.composer.value,text);assert.match(setup.setupIssues.value[0].message,/credentials_id/);assert.equal(setup.setupIssues.value[0].path,'/text');assert.equal(setup.sendFailed.value,true);assert.match(setup.sendFeedback.value,/Запрос не отправлен/);assert.equal(setup.conversation.value.length,0);assert.equal(ctx.calls.some(c=>c.path==='/api/setup/message'),false);assert.equal([...ctx.memory.values()].some(v=>v.includes(text)),false);}finally{ctx.close();}
    }
});
test('client and server share value-free credential instruction cases; assignments still stay private',()=>{
    for(const c of credentialCases)assert.equal(containsPrivateCredentials(c.text),c.blocked,c.text);
});
test('long owner prompt with value-free credential instructions is sent once and has immediate feedback while pending',async()=>{
    const entered=defer(),finish=defer();
    const text='Подготовь три тестовые зоны и четыре устройства.\n'+'Существующие настройки сохрани, команды не отправляй.\n'.repeat(60)+'Если требуется авторизация, не придумывай логин и пароль. Оставь этот вопрос для меня.';
    const ctx=await fixture(async(path,body,c)=>{assert.equal(path,'/api/setup/message');assert.equal(body.text,text);entered.resolve();await finish.promise;return c.state={...c.state,version:1,configuration:candidate()};});
    try {
        setup.composer.value=text;const pending=setup.sendMessage();await entered.promise;
        assert.equal(setup.setupBusy.value,true);assert.equal(setup.messagePending.value,true);assert.equal(setup.sendFailed.value,false);assert.match(setup.sendFeedback.value,/обрабатывается AI/);
        await setup.sendMessage();assert.equal(ctx.calls.filter(c=>c.path==='/api/setup/message').length,1,'duplicate submit is ignored while pending');
        finish.resolve();await pending;assert.equal(setup.setupBusy.value,false);assert.equal(setup.messagePending.value,false);assert.equal(setup.sendFailed.value,false);assert.ok(setup.proposal.value);assert.match(setup.sendFeedback.value,/AI подготовил предложение/);
        assert.equal(ctx.calls.some(c=>/apply|command|probe|poll/.test(c.path)),false);
    }finally{finish.resolve();ctx.close();}
});
test('server failure after submission does not claim the request was never sent or retry it',async()=>{
    const ctx=await fixture(()=>({ok:false,errors:[{code:'setup_failed',path:'/text',message:'test model failure'}]}));
    try {setup.composer.value='Создай лампу';await setup.sendMessage();assert.equal(setup.sendFailed.value,true);assert.match(setup.sendFeedback.value,/Готовое предложение не получено/);assert.doesNotMatch(setup.sendFeedback.value,/Запрос не отправлен/);assert.equal(ctx.calls.filter(c=>c.path==='/api/setup/message').length,1);}finally{ctx.close();}
});
test('completed proposal after reload during planning is recovered for review without acceptance or another AI request',async()=>{
    const ctx=await fixture(()=>{throw Error('Unexpected mutation');});
    try {
        const saved=JSON.parse(ctx.memory.get('firefly-v2-setup-owner'));saved.sourceText='Создай лампу';saved.retryText=saved.sourceText;ctx.memory.set('firefly-v2-setup-owner',JSON.stringify(saved));
        ctx.state={...ctx.state,status:'planning',message_incomplete:true,progress:{phase:'planning',step:1,total_steps:1}};
        await setup.restoreSession();assert.equal(setup.proposal.value,null);assert.equal(setup.composer.value,'Создай лампу');
        ctx.state={...ctx.state,status:'editing',message_incomplete:false,version:1,configuration:candidate(),proposal_scope:'configuration'};
        await setup.refreshSession();assert.ok(setup.proposal.value);assert.equal(setup.proposal.value.version,1);assert.equal(manual.candidate.value.devices.length,0);assert.equal(ctx.calls.some(c=>/setup\/message|apply|command|probe|poll/.test(c.path)),false);
    }finally{ctx.close();}
});
