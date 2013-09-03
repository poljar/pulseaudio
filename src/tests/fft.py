#!/usr/bin/env python3

import matplotlib.pyplot as plt
from scipy.io import wavfile
import argparse
from numpy import fft, arange, sin, pi, log, hamming, array, abs, ceil

def getFft(samples, rate):
    # TODO add support for different window types
    window = hamming(len(samples))

    Y = fft.fft(samples * window)
    n = len(samples)
    freq = fft.fftfreq(n, 1.0 / rate)

    # Use only the postive side of the frequnecy spectrum
    n = ceil(n / 2)
    # Normalize
    Y = (2.0 / n) * Y[:n]
    freq = freq[:n]

    return Y, freq

# TODO make this plot intelligent
def plotFft(Y, freq):
    plt.plot(10 * log(abs(Y)))
    plt.xscale('log')
    plt.show()

def findFftPeak(Y, freq):
    max1 = 0
    max2 = 0
    maxfreq = 0
    maxfreq2 = 0

    magnitude = abs(Y)

    # TODO this does note actually find the second peak
    # scipy.signal.find_peaks_cwt could be handy or maybe
    # do a FFT with zero-padding
    for i in range(len(magnitude)):
        if magnitude[i] > max1:
            max1 = magnitude[i]
            maxfreq = freq[i]

    for i in range(len(magnitude)):
        if magnitude[i] > max2 and magnitude[i] != max1:
            max2 = magnitude[i]
            maxfreq2 = freq[i]

    print("First peak: ", 10 * log(max1), "dB ",  maxfreq, "Hz")
    print("Second peak: ", 10 * log(max2), "dB ",  maxfreq2, "Hz")
    print("SNR: ", 20 * log(max1 / max2))

def readWav(fileName):
    wv = wavfile.read(fileName)

    rate = wv[0]
    samples = wv[1]

    # scipy.io.wavfile does not support float so convert s16 and friends to 64 bit float here
    # TODO add support for more formats (probably should use 24 or 32 bit int as default (if it's supported)
    if samples.dtype == 'int16':
        samples = samples / (float(2**15) + 1)
    else:
        raise NotImplementedError('We currently support only 16 bit PCM for the input file format!')

    return samples, rate

# This is similar to audacity's export spectrum functionality
def dumpFFT(Y, freq):
    logMagnitude = 10 * log(abs(Y))

    print("Frequency (Hz)\tLevel (dB)")

    for level, f in zip(logMagnitude, freq):
        print(f, "\t", level)

def parseArgs():
    parser = argparse.ArgumentParser(description = 'Spectral analysis for WAV files')
    parser.add_argument('command', help = 'Operations to use', choices = ['plot', 'snr', 'dump'], nargs='+')
    parser.add_argument('file', help = 'WAV file to inspect must be 16 bit PCM')

    return parser.parse_args()

def main():
    args = parseArgs()

    fileName = args.file

    samples, rate = readWav(fileName)
    Y, freq = getFft(samples, rate)

    for command in args.command:
        if command == 'plot':
            plotFft(Y, freq)
        elif command == 'snr':
            findFftPeak(Y, freq)
        elif command == 'dump':
            dumpFFT(Y, freq)
        else:
            # Should not happen since argparse handles this case
            raise ValueError('Unsuported command')

if __name__ == "__main__":
    main()
