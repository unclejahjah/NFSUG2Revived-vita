#include <lib/audio.h>
#include <platform/config.h>
#include <platform/cores.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/time.h>
#ifdef __vita__
#include <psp2/audioout.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

// free roam's sounds (src/lib/winapi/revived/roam/roam_vita.cpp): added to each block of the mix; weak, for builds
// without free roam
extern "C" void roam_vita_audio_mix(int32_t* acc, unsigned frames, unsigned rate) __attribute__((weak));

namespace win32
{

namespace
{

// Hundredths of a decibel to a Q15 gain; -10000 and below is silence.
x86::sreg32 attenuationToGain(x86::sreg32 hundredthsDb)
{
    if (hundredthsDb <= -10000)
        return 0;
    if (hundredthsDb >= 0)
        return AudioDevice::s_unityGain;
    return x86::sreg32(std::pow(10.0f, float(hundredthsDb) / 2000.0f) * float(AudioDevice::s_unityGain) + 0.5f);
}

inline x86::sreg32 sampleValue(x86::reg8 sample)    { return (x86::sreg32(sample) - 128) * 256; }
inline x86::sreg32 sampleValue(x86::sreg16 sample)  { return sample; }

// Mixes up to `frames` output frames from at most `available` source frames starting at `index` in a
// ring of `ringFrames`, interpolating linearly between source frames. Advances index and phase and
// returns the number of source frames consumed.
template <typename Sample, x86::reg32 Channels>
x86::reg32 mixFrames(const Sample* ring, x86::reg32 ringFrames, x86::reg32& index, x86::reg32 available,
                     x86::reg32& phase, x86::reg32 step, x86::sreg32 gainLeft, x86::sreg32 gainRight,
                     x86::sreg32* out, x86::reg32 frames)
{
    x86::reg32 consumed = 0;
    for (x86::reg32 i = 0; i < frames && consumed < available; ++i, out += AudioDevice::s_channelCount)
    {
        const Sample* current = ring + index * Channels;
        const Sample* next = current;
        if (consumed + 1 < available)
            next = index + 1 == ringFrames ? ring : current + Channels;
        const x86::sreg32 fraction = x86::sreg32(phase >> 1);
        x86::sreg32 left = sampleValue(current[0]);
        left += ((sampleValue(next[0]) - left) * fraction) >> 15;
        x86::sreg32 right = left;
        if constexpr (Channels == 2)
        {
            right = sampleValue(current[1]);
            right += ((sampleValue(next[1]) - right) * fraction) >> 15;
        }
        out[0] += (left * gainLeft) >> 15;
        out[1] += (right * gainRight) >> 15;

        phase += step;
        x86::reg32 advance = phase >> 16;
        phase &= 0xffff;
        if (advance > available - consumed)
        {
            advance = available - consumed;
            phase = 0;
        }
        consumed += advance;
        index += advance;
        while (index >= ringFrames)
            index -= ringFrames;
    }
    return consumed;
}

// Advances index and phase like mixFrames without reading samples.
x86::reg32 skipFrames(x86::reg32 ringFrames, x86::reg32& index, x86::reg32 available,
                      x86::reg32& phase, x86::reg32 step, x86::reg32 frames)
{
    const x86::reg64 end = x86::reg64(phase) + x86::reg64(step) * frames;
    x86::reg32 consumed = x86::reg32(end >> 16);
    phase = x86::reg32(end & 0xffff);
    if (consumed >= available)
    {
        consumed = available;
        phase = 0;
    }
    index = (index + consumed) % ringFrames;
    return consumed;
}

// Rates SceAudioOut accepts on a BGM port.
const x86::reg32 s_portRates[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 };
const x86::reg32 s_fallbackRate = 48000;
// Output block length (audio_block_ms); the game's mixer renders about one block per wake.
x86::reg32 blockMs()
{
    static const x86::reg32 s_ms = x86::reg32(std::min(std::max(pf::Config::getInt("audio_block_ms", 20), 5), 40));
    return s_ms;
}

bool s_audioLog = false;
std::atomic<unsigned> s_nextBufferId(0);

#ifndef __vita__
const x86::reg32 SCE_AUDIO_MIN_LEN = 64;

// Host port of the one device: the sink gets each block, then the call waits as sceAudioOutOutput does, until the
// block queued before it starts to play.
struct HostPort
{
    AudioSink   sink = nullptr;
    void*       context = nullptr;
    x86::reg32  rate = 0;
    x86::reg32  grain = 0;
    uint64_t    startUs = 0;    // play time of the first block since the last underrun
    uint64_t    frames = 0;     // frames queued since then
};
HostPort s_hostPort;

void hostConfigure(x86::reg32 grain, x86::reg32 rate)
{
    s_hostPort.grain = grain;
    s_hostPort.rate = rate;
    s_hostPort.frames = 0;
}

void hostOutput(const x86::sreg16* samples)
{
    HostPort& port = s_hostPort;
    if (port.sink)
        port.sink(samples, port.grain, port.rate, port.context);
    const uint64_t now = pf::ticksUs();
    uint64_t end = port.startUs + port.frames * 1000000 / port.rate;
    if (port.frames == 0 || end < now)
    {
        port.startUs = now;
        port.frames = 0;
        end = now;
    }
    const uint64_t blockUs = uint64_t(port.grain) * 1000000 / port.rate;
    if (end > now + blockUs)
        pf::sleepUs(uint32_t(end - blockUs - now));
    port.frames += port.grain;
}
#endif

}

#ifndef __vita__
void setAudioSink(AudioSink sink, void* context)
{
    s_hostPort.sink = sink;
    s_hostPort.context = context;
}
#endif

AudioDevice::AudioDevice()
    :   m_port(-1)
    ,   m_mutex("audio", false)
    ,   m_rate(0)
    ,   m_grain(0)
    ,   m_requestedRate(s_defaultRate)
    ,   m_running(false)
    ,   m_thread(nullptr)
{
    m_masterGain[0] = s_unityGain;
    m_masterGain[1] = s_unityGain;
    s_audioLog = pf::Config::getBool("audio_log", false);
    if (!configurePort(portRate(s_defaultRate)) && !configurePort(s_fallbackRate))
        return;
    m_running = true;
    m_thread = new pf::Thread("audio",&threadEntry, this, pf::Thread::PriorityHigh, 64 * 1024,
                              pf::cores::mask(pf::cores::Audio));
    m_thread->start();
}

AudioDevice::~AudioDevice()
{
    if (m_thread)
    {
        m_running = false;
        m_thread->join();
        delete m_thread;
    }
#ifdef __vita__
    if (m_port >= 0)
    {
        sceAudioOutOutput(m_port, nullptr);
        sceAudioOutReleasePort(m_port);
    }
#endif
}

x86::reg32 AudioDevice::portRate(x86::reg32 sampleRate)
{
    for (x86::reg32 rate : s_portRates)
    {
        if (rate == sampleRate)
            return rate;
    }
    return s_fallbackRate;
}

// A multiple of 64 frames (SceAudioOut granularity) close to audio_block_ms.
x86::reg32 AudioDevice::grainFrames(x86::reg32 sampleRate)
{
    const x86::reg32 frames = (sampleRate * blockMs() / 1000 + 63) & ~63u;
    return std::min(std::max<x86::reg32>(frames, SCE_AUDIO_MIN_LEN), s_maxGrain);
}

// Opens the port, or changes its rate. Audio thread, or the constructor before it starts.
bool AudioDevice::configurePort(x86::reg32 rate)
{
    const x86::reg32 grain = grainFrames(rate);
    int result;
#ifdef __vita__
    if (m_port < 0)
    {
        result = m_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, grain, rate, SCE_AUDIO_OUT_MODE_STEREO);
        if (m_port >= 0)
        {
            int volume[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
            sceAudioOutSetVolume(m_port, SceAudioOutChannelFlag(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH),
                                 volume);
        }
    }
    else
    {
        result = sceAudioOutSetConfig(m_port, grain, rate, SCE_AUDIO_OUT_MODE_STEREO);
    }
#else
    result = m_port = 0;
    hostConfigure(grain, rate);
#endif
    if (result < 0)
    {
        pf::logError("audio: output at %u Hz failed: 0x%08x; not tried again", rate, result);
        m_rejectedRates.push_back(rate);
        return false;
    }
    pf::LockGuard guard(m_mutex);
    m_rate = rate;
    m_grain = grain;
    pf::logInfo("audio: output %u Hz, %u frames per block", rate, grain);
    return true;
}

void AudioDevice::setOutputRate(x86::reg32 sampleRate)
{
    m_requestedRate.store(sampleRate);
}

void AudioDevice::play(AudioBuffer* buffer, bool looping)
{
    pf::LockGuard guard(m_mutex);
    if (s_audioLog)
        pf::logInfo("audio: t=%u ms play #%u%s%s", unsigned(pf::ticksMs()), buffer->m_id, looping ? " looping" : "",
                    buffer->m_playing ? " (already playing)" : "");
    buffer->m_looping = looping;
    if (!buffer->m_playing)
    {
        buffer->m_playing = true;
        m_playingBuffers.push_back(buffer);
    }
}

void AudioDevice::stop(AudioBuffer* buffer)
{
    pf::LockGuard guard(m_mutex);
    if (s_audioLog && buffer->m_playing)
        pf::logInfo("audio: t=%u ms stop #%u", unsigned(pf::ticksMs()), buffer->m_id);
    buffer->m_playing = false;
    m_playingBuffers.erase(std::remove(m_playingBuffers.begin(), m_playingBuffers.end(), buffer), m_playingBuffers.end());
}

int AudioDevice::threadEntry(void* self)
{
    if (pf::perf::enabled())
        pf::perf::trackThread("audio");
    static_cast<AudioDevice*>(self)->run();
    return 0;
}

// sceAudioOutOutput returns once the port has room for the block, so the loop runs at the output rate and the
// buffers' play cursors advance in real time.
void AudioDevice::run()
{
    // timing_log: output level and playing buffers about every 5 s.
    const bool levels = pf::perf::enabled() || s_audioLog;
    const x86::reg32 reportSeconds = s_audioLog ? 1 : 5;
    x86::reg32 lastRate = m_rate;
    x86::reg32 levelFrames = 0;
    x86::reg32 peak = 0;
    double squares = 0.0;
    x86::reg32 fullScale = 0;
    x86::reg32 saturated = 0;
    while (m_running)
    {
        x86::reg32 wanted = portRate(m_requestedRate.load());
        if (std::find(m_rejectedRates.begin(), m_rejectedRates.end(), wanted) != m_rejectedRates.end())
            wanted = m_rate;
        if (wanted != m_rate && !configurePort(wanted))
            wanted = m_rate;
        const x86::reg32 samples = m_grain * s_channelCount;
        memset(m_accumulator, 0, samples * sizeof(m_accumulator[0]));
        {
            pf::LockGuard guard(m_mutex);
            for (std::vector<AudioBuffer*>::iterator it = m_playingBuffers.begin(); it != m_playingBuffers.end();)
            {
                if ((*it)->mixInto(m_accumulator, m_grain, m_rate, m_masterGain))
                    ++it;
                else
                    it = m_playingBuffers.erase(it);
            }
        }
        // free roam's horns and the other players' engines, nitrous and hydraulics over the game's sound
        if (roam_vita_audio_mix)
            roam_vita_audio_mix(reinterpret_cast<int32_t*>(m_accumulator), m_grain, m_rate);
        for (x86::reg32 i = 0; i < samples; ++i)
        {
            x86::sreg32 v = m_accumulator[i];
            m_output[i] = x86::sreg16(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }
        if (levels)
        {
            size_t playing;
            std::string list;
            {
                pf::LockGuard guard(m_mutex);
                playing = m_playingBuffers.size();
                if (s_audioLog)
                {
                    char text[96];
                    for (const AudioBuffer* b : m_playingBuffers)
                    {
                        snprintf(text, sizeof(text), " #%u%s%s %uHz pos %u/%u written %u ago", b->m_id,
                                 b->m_primary ? "P" : "", b->m_looping ? "L" : "", b->frequency(), b->m_position,
                                 b->m_bufferSize, b->m_staleFrames);
                        list += text;
                    }
                }
            }
            for (x86::reg32 i = 0; i < samples; ++i)
            {
                const x86::sreg32 v = m_accumulator[i];
                const x86::reg32 magnitude = x86::reg32(v < 0 ? -v : v);
                peak = std::max(peak, magnitude);
                squares += double(v) * double(v);
                fullScale += v >= 32767 || v <= -32768;
                saturated += v > 32767 || v < -32768;
            }
            levelFrames += lastRate == m_rate ? m_grain : 0;
            if (levelFrames >= m_rate * reportSeconds)
            {
                pf::logInfo("audio: t=%u ms peak %u rms %.0f full scale %u saturated %u over %u frames, %u buffers playing%s",
                            unsigned(pf::ticksMs()), peak, std::sqrt(squares / (double(levelFrames) * s_channelCount)),
                            fullScale, saturated, levelFrames, unsigned(playing), list.c_str());
                levelFrames = 0;
                peak = 0;
                squares = 0.0;
                fullScale = 0;
                saturated = 0;
            }
            lastRate = m_rate;
        }
#ifdef __vita__
        sceAudioOutOutput(m_port, m_output);
#else
        hostOutput(m_output);
#endif
    }
}

AudioBuffer::AudioBuffer(AudioDevice* device, WinApplication* app, x86::reg32 bufferSize, const AudioFormat& format, bool primary)
    :   m_device(device)
    ,   m_bufferSize(bufferSize)
    ,   m_memmap(new MemMap(bufferSize, MemMap::Low, pf::perf::AllocSound))
    ,   m_data(&app->getMemory<x86::reg8>(m_memmap->getBlockStart()))
    ,   m_format(format)
    ,   m_position(0)
    ,   m_staleFrames(0)
    ,   m_phase(0)
    ,   m_frequency(0)
    ,   m_volume(0)
    ,   m_pan(0)
    ,   m_playing(false)
    ,   m_looping(false)
    ,   m_primary(primary)
{
    m_gain[0] = AudioDevice::s_unityGain;
    m_gain[1] = AudioDevice::s_unityGain;
    m_id = ++s_nextBufferId;
    if (s_audioLog)
        pf::logInfo("audio: t=%u ms create #%u %s%u bytes %u Hz %u ch %u bit", unsigned(pf::ticksMs()), m_id,
                    primary ? "primary " : "", bufferSize, format.sampleRate, format.channels, format.bitsPerSample);
    // A new buffer is zeroed, as on Windows.
    memset(&app->getMemory<x86::reg8>(m_memmap->getBlockStart()), 0, m_bufferSize);
}

AudioBuffer::~AudioBuffer()
{
    if (s_audioLog)
        pf::logInfo("audio: t=%u ms destroy #%u (%s)", unsigned(pf::ticksMs()), m_id, m_playing ? "was playing" : "stopped");
    m_device->stop(this);
    delete m_memmap;
}

void AudioBuffer::play(bool looping)
{
    m_device->play(this, looping);
}

void AudioBuffer::stop()
{
    m_device->stop(this);
}

bool AudioBuffer::playing() const
{
    pf::LockGuard guard(m_device->mutex());
    return m_playing;
}

x86::reg32 AudioBuffer::lock() const
{
    return m_memmap->getBlockStart();
}

void AudioBuffer::unlock()
{
    pf::LockGuard guard(m_device->mutex());
    m_staleFrames = 0;
}

x86::reg32 AudioBuffer::position() const
{
    pf::LockGuard guard(m_device->mutex());
    return m_position;
}

void AudioBuffer::setPosition(x86::reg32 offset)
{
    pf::LockGuard guard(m_device->mutex());
    m_position = offset - offset % m_format.frameBytes();
    m_phase = 0;
    if (s_audioLog)
        pf::logInfo("audio: t=%u ms position #%u = %u", unsigned(pf::ticksMs()), m_id, m_position);
}

void AudioBuffer::setFormat(const AudioFormat& format)
{
    pf::LockGuard guard(m_device->mutex());
    m_format = format;
    m_position -= m_position % format.frameBytes();
    m_phase = 0;
    if (s_audioLog)
        pf::logInfo("audio: t=%u ms format #%u %u Hz %u ch %u bit", unsigned(pf::ticksMs()), m_id, format.sampleRate,
                    format.channels, format.bitsPerSample);
    if (m_primary)
        m_device->setOutputRate(format.sampleRate);
}

void AudioBuffer::setVolume(x86::sreg32 volume)
{
    pf::LockGuard guard(m_device->mutex());
    m_volume = volume;
    updateGain();
}

void AudioBuffer::setPan(x86::sreg32 pan)
{
    pf::LockGuard guard(m_device->mutex());
    m_pan = pan;
    updateGain();
}

void AudioBuffer::setFrequency(x86::reg32 frequency)
{
    pf::LockGuard guard(m_device->mutex());
    m_frequency = frequency;
}

// Pan attenuates the opposite channel. The primary buffer's volume and pan apply to the whole mix.
void AudioBuffer::updateGain()
{
    x86::sreg32* gain = m_primary ? m_device->m_masterGain : m_gain;
    gain[0] = attenuationToGain(m_volume - std::max<x86::sreg32>(m_pan, 0));
    gain[1] = attenuationToGain(m_volume + std::min<x86::sreg32>(m_pan, 0));
}

bool AudioBuffer::mixInto(x86::sreg32* accumulator, x86::reg32 frames, x86::reg32 outputRate,
                          const x86::sreg32* masterGain)
{
    // Source frames per output frame, 16.16 fixed point.
    const x86::reg32 step = x86::reg32((x86::reg64(frequency()) << 16) / outputRate);
    const x86::reg32 frameBytes = m_format.frameBytes();
    const x86::reg32 ringFrames = m_bufferSize / frameBytes;
    x86::reg32 index = m_position / frameBytes;
    // Frames left before a non-looping buffer ends.
    const x86::reg32 available = m_looping ? ~x86::reg32(0) : ringFrames - index;
    // The primary buffer is only written as a write-primary stream; a lap without new data plays silent.
    const bool stale = m_primary && m_staleFrames >= ringFrames;
    const x86::sreg32 left = stale ? 0 : (m_gain[0] * masterGain[0]) >> 15;
    const x86::sreg32 right = stale ? 0 : (m_gain[1] * masterGain[1]) >> 15;
    x86::reg32 consumed;
    if (!left && !right)
        consumed = skipFrames(ringFrames, index, available, m_phase, step, frames);
    else if (m_format.bitsPerSample == 16)
    {
        const x86::sreg16* ring = reinterpret_cast<const x86::sreg16*>(m_data);
        if (m_format.channels == 2)
            consumed = mixFrames<x86::sreg16, 2>(ring, ringFrames, index, available, m_phase, step, left, right, accumulator, frames);
        else
            consumed = mixFrames<x86::sreg16, 1>(ring, ringFrames, index, available, m_phase, step, left, right, accumulator, frames);
    }
    else
    {
        if (m_format.channels == 2)
            consumed = mixFrames<x86::reg8, 2>(m_data, ringFrames, index, available, m_phase, step, left, right, accumulator, frames);
        else
            consumed = mixFrames<x86::reg8, 1>(m_data, ringFrames, index, available, m_phase, step, left, right, accumulator, frames);
    }
    m_staleFrames = std::min(m_staleFrames + consumed, ringFrames);
    m_position = index * frameBytes;
    if (consumed < available)
        return true;
    m_playing = false;
    m_phase = 0;
    if (s_audioLog)
        pf::logInfo("audio: t=%u ms end #%u", unsigned(pf::ticksMs()), m_id);
    return false;
}


}
