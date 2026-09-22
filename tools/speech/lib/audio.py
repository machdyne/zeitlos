"""
Zeitlos
Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.

Reading recordings and measuring them: the spectral features the
aligner compares, and the pitch the prosody models are fitted to.

numpy only, deliberately. The training run happens on somebody's own
machine against a few hundred megabytes of audio, and an extra
dependency is an extra thing to go wrong there; everything needed is
a Fourier transform and some arithmetic.
"""

import struct
import wave

import numpy as np

FS = 11025          # the rate sw/apps/tts synthesises at
HOP = 110           # 10ms frames at FS
WIN = 256


def read_wav(path):
    """Mono float samples at FS. LJSpeech is 22050Hz: exactly 2x."""

    w = wave.open(path, "rb")
    rate, width, chans, n = w.getframerate(), w.getsampwidth(), w.getnchannels(), w.getnframes()
    raw = w.readframes(n)
    w.close()

    if width != 2:
        raise ValueError("%s: %d-byte samples, expected 16-bit" % (path, width))

    x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    if chans > 1:
        x = x.reshape(-1, chans).mean(axis=1)

    if rate == FS:
        return x
    if rate == 2 * FS:
        # Low-pass before dropping every other sample, or everything
        # between 5.5 and 11kHz folds back down into the speech band.
        # A 31-tap windowed sinc at a quarter of the input rate is
        # plenty for alignment and pitch.
        t = np.arange(-15, 16)
        h = np.sinc(t / 2.0) * np.hamming(31)
        h /= h.sum()
        return np.convolve(x, h, mode="same")[::2]

    # Anything else: linear interpolation. Not great audio, fine for
    # what is measured here.
    dur = len(x) / float(rate)
    t_new = np.arange(int(dur * FS)) / float(FS)
    return np.interp(t_new, np.arange(len(x)) / float(rate), x)


def frames(x):
    """Overlapping, windowed frames: one per HOP samples."""
    if len(x) < WIN:
        x = np.pad(x, (0, WIN - len(x)))
    n = 1 + (len(x) - WIN) // HOP
    idx = np.arange(WIN)[None, :] + HOP * np.arange(n)[:, None]
    return x[idx] * np.hanning(WIN)[None, :]


def _mel_bank(nbands=24):
    """Triangular bands, equally spaced on the mel scale, 80Hz-5kHz."""
    def mel(f):
        return 2595.0 * np.log10(1.0 + f / 700.0)

    def hz(m):
        return 700.0 * (10 ** (m / 2595.0) - 1.0)

    edges = hz(np.linspace(mel(80.0), mel(5000.0), nbands + 2))
    bins = np.fft.rfftfreq(WIN, 1.0 / FS)
    bank = np.zeros((nbands, len(bins)))
    for b in range(nbands):
        lo, mid, hi = edges[b], edges[b + 1], edges[b + 2]
        up = (bins - lo) / (mid - lo)
        down = (hi - bins) / (hi - mid)
        bank[b] = np.clip(np.minimum(up, down), 0.0, None)
    return bank


_BANK = _mel_bank()
_DCT = np.cos(np.pi / 24.0 * (np.arange(24)[None, :] + 0.5) * np.arange(13)[:, None])


def features(x):
    """
    Cepstra per 10ms frame, mean-normalised over the utterance.

    What the aligner compares. The normalisation is what lets it
    compare two very different voices -- ours and a recorded speaker --
    at all: it removes each one's average spectral colour and leaves
    the changes over time, which is where the phonemes are.
    """
    spec = np.abs(np.fft.rfft(frames(x), axis=1)) ** 2
    bands = np.log(spec @ _BANK.T + 1e-10)
    cep = bands @ _DCT.T
    cep = cep[:, 1:]                        # drop c0: loudness, not phoneme
    cep -= cep.mean(axis=0, keepdims=True)
    # Deltas: the direction things are moving, which is what
    # distinguishes a transition from a steady state.
    d = np.zeros_like(cep)
    d[1:-1] = (cep[2:] - cep[:-2]) / 2.0
    return np.hstack([cep, d])


def energy(x):
    """Frame energy in dB, for finding where speech starts and stops."""
    f = frames(x)
    return 10.0 * np.log10((f ** 2).mean(axis=1) + 1e-12)


def speech_span(x, floor_db=35.0):
    """
    First and last frame holding speech: frames within `floor_db` of
    the loudest. Recordings have silence at each end that the reference
    does not, and aligning silence to speech drags every boundary.
    """
    e = energy(x)
    on = np.where(e > e.max() - floor_db)[0]
    if len(on) == 0:
        return 0, len(e)
    return int(on[0]), int(on[-1]) + 1


def pitch(x, lo=60.0, hi=400.0):
    """
    Fundamental frequency per 10ms frame, 0 where unvoiced.

    Normalised autocorrelation over a 40ms window with a voicing
    threshold, plus a check against octave errors: the classic failure
    of autocorrelation is picking twice the period, and a pitch track
    that jumps an octave for one frame poisons every average taken
    from it.
    """
    win = int(0.040 * FS)
    lag_lo, lag_hi = int(FS / hi), int(FS / lo)
    n = max(0, 1 + (len(x) - win) // HOP)
    f0 = np.zeros(n)
    e = energy(x)
    quiet = e.max() - 30.0

    for i in range(n):
        if i < len(e) and e[i] < quiet:
            continue
        seg = x[i * HOP:i * HOP + win]
        seg = seg - seg.mean()
        denom = np.dot(seg, seg)
        if denom <= 0:
            continue
        ac = np.correlate(seg, seg, "full")[win - 1:]
        ac = ac / denom
        if lag_hi >= len(ac):
            continue
        region = ac[lag_lo:lag_hi]
        k = int(np.argmax(region)) + lag_lo
        if ac[k] < 0.45:
            continue
        # Prefer the shorter period if it is nearly as strong: guards
        # against picking a multiple of the true period.
        half = k // 2
        if half >= lag_lo and ac[half] > 0.85 * ac[k]:
            k = half
        f0[i] = FS / float(k)

    # One-frame spikes more than 30% off both neighbours are errors.
    for i in range(1, n - 1):
        a, b, c = f0[i - 1], f0[i], f0[i + 1]
        if a and b and c and abs(b - a) / a > 0.3 and abs(b - c) / c > 0.3:
            f0[i] = (a + c) / 2.0

    return f0


def write_wav(path, x, rate=FS):
    y = np.clip(x, -1.0, 1.0)
    data = (y * 32767.0).astype("<i2").tobytes()
    w = wave.open(path, "wb")
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(rate)
    w.writeframes(data)
    w.close()


def _lpc(frame, order):
    """Linear prediction by autocorrelation and Levinson-Durbin."""
    r = np.correlate(frame, frame, "full")[len(frame) - 1:len(frame) + order]
    if r[0] <= 0:
        return None
    a = np.zeros(order + 1)
    a[0] = 1.0
    e = r[0]
    for i in range(1, order + 1):
        k = -(r[i] + np.dot(a[1:i], r[i - 1:0:-1])) / e
        a[1:i] = a[1:i] + k * a[i - 1:0:-1]
        a[i] = k
        e *= (1.0 - k * k)
        if e <= 0:
            return None
    return a


def formants(x, order=12):
    """
    The first three formants per 10ms frame, in Hz, 0 where there is
    nothing to measure.

    Linear prediction: fit an all-pole filter to each 25ms frame, and
    read the resonances off its poles. Twelve poles at 11025Hz is the
    textbook order for an adult voice -- two per kHz, plus a few for the
    glottal and radiation slopes. Poles too wide to be a formant (over
    400Hz) or outside 90-5000Hz are discarded.

    It is biased -- LPC always is, and more so on a high voice -- which
    is why tools/speech never uses these numbers raw: the same tracker
    is run on our own voice and the speaker's value is taken relative
    to that (lib/acoustics.py).
    """

    win = int(0.025 * FS)
    n = max(0, 1 + (len(x) - win) // HOP)
    out = np.zeros((n, 3))
    if n == 0:
        return out

    y = np.append(x[0], x[1:] - 0.97 * x[:-1])      # pre-emphasis
    e = energy(x)
    quiet = e.max() - 35.0
    w = np.hamming(win)

    for i in range(n):
        if i < len(e) and e[i] < quiet:
            continue
        seg = y[i * HOP:i * HOP + win]
        if len(seg) < win:
            break
        a = _lpc(seg * w, order)
        if a is None:
            continue
        roots = np.roots(a)
        roots = roots[np.imag(roots) > 0]
        if len(roots) == 0:
            continue
        freq = np.angle(roots) * FS / (2 * np.pi)
        bw = -np.log(np.abs(roots) + 1e-12) * FS / np.pi
        ok = (freq > 90) & (freq < 5000) & (bw < 400)
        f = np.sort(freq[ok])
        k = min(3, len(f))
        out[i, :k] = f[:k]

    return out
