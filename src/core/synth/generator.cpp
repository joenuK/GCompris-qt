/* miniSynth - A Simple Software Synthesizer
   Copyright (C) 2015 Ville Räisänen <vsr at vsr.name>

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "generator.h"
#include "filter.h"
#include <qendian.h>

Generator::Generator(const QAudioFormat &_format, QObject *parent) : QIODevice(parent) {
    format = _format;
    linSyn = new LinearSynthesis(Waveform::MODE_SIN);
    curtime = 0;

    defaultEnv.attackTime = 100;
    defaultEnv.decayTime = 400;
    defaultEnv.releaseTime = 100;

    defaultEnv.initialAmpl = 0;
    defaultEnv.peakAmpl = 1;
    defaultEnv.sustainAmpl = 0.8;

    mod_waveform = new Waveform(Waveform::MODE_SIN);

    convImpulse_size = 0;

    convBuffer_size = 4096;
    convBuffer      = new qreal[convBuffer_size];

    convBuffer_ind  = 0;

    for (unsigned int indconv = 0; indconv < convBuffer_size; indconv++) {
        convBuffer[indconv] = 0;
    }

    filter      = nullptr;
    convImpulse = nullptr;

    synthData    = new qreal[maxUsedBytes];
    filteredData = new qreal[maxUsedBytes];

    for (unsigned int indMaxRead = 0; indMaxRead < maxUsedBytes; indMaxRead++) {
        synthData[indMaxRead]    = 0;
        filteredData[indMaxRead] = 0;
    }

    FilterParameters param;
    param.freq1 = param.freq2 = 0;
    param.samplingRate = m_samplingRate;
    param.size         = 128;
    param.type         = Filter::FILTER_OFF;
    param.window_type  = Filter::WINDOW_RECT;
    setFilter(param);
}

Generator::~Generator() {
    delete linSyn;
    delete [] convBuffer;
    delete [] convImpulse;
    delete [] synthData;
    delete [] filteredData;
    delete filter;
    delete mod_waveform;
}

void
Generator::start() {
    open(QIODevice::ReadOnly);
}

void
Generator::stop() {
    close();
}

void
Generator::addWave(unsigned char note, unsigned char vel) {
    Wave wav;
    wav.state = wav.STATE_ATTACK;
    wav.note  = note;
    wav.vel   = vel;
    wav.state_age = 0;
    wav.age      = 0;
    wav.env = defaultEnv;

    m_lock.lock();
    waveList.push_back(wav);
    m_lock.unlock();
}

qint64
Generator::readData(char *data, qint64 len) {
    // QAudioOutput tends to ask large packets of data, which can lead to a
    // large delay between noteOn requests and the generation of audio. Thus,
    // in order to provide more responsive interface, the packet size is
    // limited to 2048 bytes ~ 1024 samples.
    if (len > maxUsedBytes) len = maxUsedBytes;

    generateData(len);
    memcpy(data, m_buffer.constData(), len);
    curtime += (qreal)len/(m_samplingRate*2);
    return len;
}

// Not used.
qint64
Generator::writeData(const char *data, qint64 len) {
    Q_UNUSED(data);
    Q_UNUSED(len);
    return 0;
}

// Doesn't seem to be called by QAudioOutput.
qint64
Generator::bytesAvailable() const {
    return m_buffer.size() + QIODevice::bytesAvailable();
}

void
Generator::noteOn(unsigned char chan, unsigned char note, unsigned char vel) {
    // Velocity of 255 is assumed since a "pleasant" relationship between the
    // velocity in the MIDI event and the parameters of the corresponding Wave
    // cannot be currently selected by the user.

    if (vel > 0) vel = 255;
    addWave(note, vel);
    Q_UNUSED(chan);
}

void
Generator::noteOff(unsigned char chan, unsigned char note) {
    QMutableListIterator<Wave> i(waveList);

    while (i.hasNext()) {
        Wave wav = i.next();
        if (wav.note == note && wav.state != Wave::STATE_RELEASE) {
            // To avoid discontinuity in the envelope, the initial value for
            // the release part of the envelope should be equal to current
            // value.

            wav.env.sustainAmpl = wav.env.eval(wav.state_age, wav.state);

            wav.state = Wave::STATE_RELEASE;
            wav.state_age = 0;
        }
        i.setValue(wav);
    }
    Q_UNUSED(chan);
}

void
Generator::setMode(int _mode) {
    delete linSyn;
    linSyn = new LinearSynthesis(_mode);
    curtime = 0;
}

void
Generator::setTimbre(QVector<int> &amplitudes, QVector<int> &phases) {
    linSyn->setTimbre(amplitudes, phases);
}

void
Generator::generateData(qint64 len) {
    unsigned int numSamples = len/2;
    m_buffer.resize(len);

    // Raw synthesized data is assembled into synthData. This data is then
    // filtered and assembled into filteredData.
    memset(synthData, 0.f, numSamples*sizeof(qreal));
    memset(filteredData, 0.f, numSamples*sizeof(qreal));

    // All samples for each active note in waveList are synthesized separately.
    m_lock.lock();
    QMutableListIterator<Wave> i(waveList);

    while (i.hasNext()) {
        Wave wav = i.next();
        qreal attackTime  = 0.001*(qreal)wav.env.attackTime,
              releaseTime = 0.001*(qreal)wav.env.releaseTime;

        qreal freq = 8.175 * 0.5 * qPow(2, ((qreal)wav.note)/12);
        qreal ampl = 0.5*((qreal)wav.vel)/256;

        qreal stateAge = wav.state_age,
              wavAge   = wav.age;

        const qreal step = 1.f / m_samplingRate;
        qreal samplePerStep = 0.f;
        for (unsigned int sample = 0; sample < numSamples; sample++, samplePerStep += step) {
            qreal t    = curtime   + samplePerStep;
            qreal envt = stateAge  + samplePerStep;
            qreal modt = wavAge    + samplePerStep;

            // Handle timed change of state in the ADSR-envelopes ATTACK->DECAY
            // and RELEASE->OFF.
            switch(wav.state) {
            case ADSREnvelope::STATE_ATTACK:
                if (envt > attackTime) {
                    stateAge -= attackTime;
                    wav.state = ADSREnvelope::STATE_DECAY;
                    wav.state_age -= attackTime;
                    envt = stateAge  + samplePerStep;
                }
                break;
            case ADSREnvelope::STATE_RELEASE:
                if (envt > releaseTime) {
                    stateAge = 0;
                    wav.state = ADSREnvelope::STATE_OFF;
                }
                break;
            }

            if (wav.state == ADSREnvelope::STATE_OFF) {
                break;
            } else {
                qreal freqmod = 0, amod = 0;

                // Compute modulation waves.

                if (mod.FM_freq > 0) {
                    qreal envVal = mod.useEnvelope ? wav.env.eval(envt, wav.state) : 1;
                    if (mod.propFreq) {
                        freqmod = mod.FM_ampl
                                * envVal* mod_waveform->eval(2*M_PI*mod.FM_freq*freq*modt);
                    } else {
                        freqmod = mod.FM_ampl
                                * mod_waveform->eval(2*M_PI*mod.FM_freq*modt);
                    }
                }
                if (mod.AM_freq > 0) {
                    amod = (1 - qExp(-modt/mod.AM_time))*mod.AM_ampl * mod_waveform->eval(2*M_PI*mod.AM_freq*t);
                }

                // Evaluate the output wave for the current note and add to the
                // output obtained with other notes.

                qreal envVal = wav.env.eval(envt, wav.state);
                qreal newVal = envVal * (ampl + amod)
                             * 0.5 * linSyn->evalTimbre(2*M_PI*(freq+freqmod)*(modt+100));

                synthData[sample] += newVal;
            }
        }
        wav.age += (qreal)numSamples/m_samplingRate;
        if (wav.state != ADSREnvelope::STATE_OFF) {
            wav.state_age += (qreal)numSamples/m_samplingRate;
            i.setValue(wav);
        }
        else {
            i.remove();
        }
    }
    m_lock.unlock();

    for (unsigned int sample = 0; sample < numSamples; sample++) {
        convBuffer[convBuffer_ind] = synthData[sample];

        for (unsigned int convind = 0; convind < convImpulse_size; convind ++) {            
            if (convImpulse[convind] != 0) {
                // The term convBuffer_size keeps the left side non-negative and avoids
                // negative results from the modulo operator.

                int bufind = (convBuffer_ind + convBuffer_size - convind) % convBuffer_size;

                filteredData[sample] += convImpulse[convind] * convBuffer[bufind];
            }
        }
        convBuffer_ind = (convBuffer_ind + 1) % convBuffer_size;
    }
    // Convert data from qreal to qint16.
    const int channelBytes = format.sampleSize() / 8;
    unsigned char *ptr = reinterpret_cast<unsigned char *>(m_buffer.data());
    for (unsigned int sample = 0; sample < numSamples; sample++) {
        if (filteredData[sample] > 1)  filteredData[sample] = 1;
        if (filteredData[sample] < -1) filteredData[sample] = -1;
        qint16 value = static_cast<qint16>(filteredData[sample] * 32767);
        qToLittleEndian<qint16>(value, ptr);
        ptr += channelBytes;
    }
}

void
Generator::setEnvelope(ADSREnvelope &env) {
    defaultEnv = env;
}

void
Generator::setModulation(Modulation &modulation) {
    if (modulation.mode != mod_waveform->mode) {
        delete mod_waveform;
        mod_waveform = new Waveform(modulation.mode);
    }
    mod = modulation;
}

void
Generator::setFilter(FilterParameters &filtParam) {
    delete filter;
    delete [] convImpulse;

    filter = new Filter(filtParam.type, filtParam.window_type, filtParam.size,
                        m_samplingRate, filtParam.freq1, filtParam.freq2);
    convImpulse_size = filter->size;
    convImpulse      = new qreal[convImpulse_size];
    for (unsigned int ind = 0; ind < convImpulse_size; ind++) {
        convImpulse[ind] = filter->IR[ind];
    }
}

void Generator::setPreset(Preset &preset) {
    setModulation(preset.mod);
    setMode(preset.waveformMode);
    setTimbre(preset.timbreAmplitudes, preset.timbrePhases);
    setEnvelope(preset.env);
    setFilter(preset.filt);
}
