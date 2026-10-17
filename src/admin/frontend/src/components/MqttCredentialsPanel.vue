<script setup lang="ts">
import { onMounted, ref } from 'vue';
import { adminRequest, issuesOf } from '../api/admin.js';

interface CredentialList { ok: true; ids: string[] }
const ids = ref<string[]>([]);
const id = ref('');
const username = ref('');
const password = ref('');
const busy = ref(false);
const notice = ref('');
const error = ref('');

async function refresh() {
    try {
        const reply = await adminRequest<CredentialList>('/api/mqtt/credentials');
        ids.value = [...reply.ids];
        error.value = '';
    } catch (e) {
        error.value = issuesOf(e).map(i => i.message).join(' ');
    }
}
onMounted(() => { void refresh(); });

async function save(rotate: boolean) {
    if (busy.value) return;
    notice.value = '';
    error.value = '';
    if (!/^[a-z][a-z0-9_]{0,63}$/.test(id.value) ||
        !username.value || !password.value || username.value.length > 256 ||
        password.value.length > 256 || /[\x00-\x1f\x7f]/.test(username.value + password.value)) {
        error.value = 'Укажите корректный ID, имя пользователя и пароль (до 256 символов без управляющих знаков).';
        return;
    }
    busy.value = true;
    try {
        await adminRequest('/api/mqtt/credentials',
            { id: id.value, username: username.value, password: password.value },
            rotate ? 'PUT' : 'POST');
        notice.value = rotate
            ? 'Пароль обновлён в защищённом хранилище. Для новых MQTT-подключений примените конфигурацию.'
            : 'Секрет создан. Используйте его ID в MQTT-подключении.';
        await refresh();
    } catch (e) {
        error.value = issuesOf(e).map(i => i.message).join(' ');
    } finally {
        // Never retain the entered plaintext credentials in a reactive draft
        // or in the browser after the single request has completed.
        username.value = '';
        password.value = '';
        busy.value = false;
    }
}
async function remove() {
    if (busy.value || !ids.value.includes(id.value)) return;
    if (!window.confirm('Удалить секрет? Операция запрещена, если ссылка ещё есть в сохранённой или резервной конфигурации.')) return;
    busy.value = true;
    error.value = '';
    notice.value = '';
    try {
        await adminRequest('/api/mqtt/credentials', { id: id.value }, 'DELETE');
        notice.value = 'Секрет удалён.';
        id.value = '';
        await refresh();
    } catch (e) {
        error.value = issuesOf(e).map(i => i.message).join(' ');
    } finally {
        password.value = '';
        busy.value = false;
    }
}
</script>

<template>
  <section class="panel" aria-labelledby="mqtt-credentials-title">
    <h2 id="mqtt-credentials-title">Учётные данные MQTT</h2>
    <p class="muted">Пароли хранятся в закрытых файлах Firefly, а в ConfigStore указывается только ID. Ни просмотр, ни выгрузка пароля не поддерживаются.</p>
    <p class="notice">Защищённый каталог должен быть предварительно подготовлен системным администратором. Это окно не создаёт его и не запускает брокер.</p>
    <div class="form-grid">
      <label>ID секрета
        <input v-model.trim="id" placeholder="relay_user" autocomplete="off" spellcheck="false" />
      </label>
      <label>Имя пользователя брокера
        <input v-model="username" autocomplete="off" spellcheck="false" />
      </label>
      <label>Новый пароль
        <input v-model="password" type="password" autocomplete="new-password" spellcheck="false" />
      </label>
    </div>
    <div class="inline-actions">
      <button type="button" :disabled="busy" @click="save(false)">Создать секрет</button>
      <button type="button" :disabled="busy || !ids.includes(id)" @click="save(true)">Сменить пароль</button>
      <button type="button" :disabled="busy || !ids.includes(id)" @click="remove">Удалить</button>
      <button type="button" :disabled="busy" @click="refresh">Обновить список</button>
    </div>
    <p v-if="notice" role="status">{{ notice }}</p>
    <p v-if="error" class="notice warning" role="alert">{{ error }}</p>
    <p class="muted">Доступные ID (сами пароли не показываются):</p>
    <p v-if="ids.length">
      <button v-for="item in ids" :key="item" type="button" :disabled="busy"
              @click="id = item">{{ item }}</button>
    </p>
    <p v-else class="muted">Секреты не обнаружены или хранилище пока недоступно.</p>
    <p class="muted">При смене пароля учтите: существующие MQTT-сессии могут продолжать работу с прежними учётными данными до переподключения или нового Apply. Отдельное управление локальным брокером будет в следующем этапе.</p>
  </section>
</template>
