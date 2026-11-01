#pragma once
#include <cerrno>
#include <cstring>
#include <atomic>
struct snd_pcm_t {int state=1;bool failDrain=false;};
inline std::atomic<int> fake_alsa_handles{0};
constexpr int SND_PCM_STREAM_PLAYBACK=0,SND_PCM_NONBLOCK=1,SND_PCM_FORMAT_S16_LE=2,SND_PCM_ACCESS_RW_INTERLEAVED=3,SND_PCM_STATE_DRAINING=4;
inline int snd_pcm_open(snd_pcm_t** p,const char* name,int,int){if(std::strcmp(name,"FAIL_PLAY")==0)return -EIO;*p=new snd_pcm_t;(*p)->failDrain=std::strcmp(name,"FAIL_DRAIN")==0;++fake_alsa_handles;return 0;}
inline int snd_pcm_set_params(snd_pcm_t*,int,int,unsigned,unsigned,int,unsigned){return 0;}
inline long snd_pcm_writei(snd_pcm_t*,const void*,unsigned long n){return n;}
inline int snd_pcm_drain(snd_pcm_t* p){p->state=0;return p->failDrain?-EIO:0;}
inline int snd_pcm_state(snd_pcm_t* p){return p->state;}
inline int snd_pcm_wait(snd_pcm_t*,int){return 1;}
inline int snd_pcm_prepare(snd_pcm_t*){return 0;}
inline int snd_pcm_drop(snd_pcm_t*){return 0;}
inline int snd_pcm_close(snd_pcm_t* p){delete p;--fake_alsa_handles;return 0;}
inline const char* snd_strerror(int){return "isolated playback failure";}
