# Copyright 2024 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

#################################################################################################
# Test FFT algorithm
#
# This is used to experiment with various step-tracking algorithms. It is a python implementation
#  of an FFT as described in:
#     "Real-valued Fast Fourier Transform Algorithm", from IEEE Transactions on Acoustics,
#       Speech, and Signal Processing, Vol. ASSP-35, No. 6, June 1987
#
# The firmware uses this same algorithm, but implemented in C in the prv_fft_2radix_real()
#  method of kraepelin_algorithm.c
##################################################################################################

import argparse
import os
import sys
import logging
import math


###########################################################################################
g_walk_10_steps = [
  [-362, -861, 69],
  [-309, -899, 45],
  [-266, -904, 21],
  [-242, -848, -134],
  [-272, -839, 34],
  [-207, -919, 14],
  [-244, -879, 93],
  [-238, -856, 91],
  [-185, -883, 37],
  [-217, -855, -156],
  [-200, -883, 25],
  [-154, -927, 42],
  [-179, -935, 71],
  [-184, -956, 32],
  [-129, -999, 99],
  [-195, -950, -112],
  [-222, -969, -164],
  [-351, -996, -190],
  [-277, -1218, -259],
  [-212, -1018, -250],
  [-209, -812, -142],
  [-182, -680, -200],
  [-257, -642, -169],
  [-269, -797, -289],
  [-142, -1107, -330],
  [-185, -909, -300],
  [-229, -706, -155],
  [-171, -750, -161],
  [-181, -811, -218],
  [-173, -845, -149],
  [-118, -887, -126],
  [-150, -871, -100],
  [-164, -908, -146],
  [-175, -958, -161],
  [-231, -952, -113],
  [-273, -1006, -205],
  [-321, -1047, -351],
  [-321, -1064, -300],
  [-262, -945, -210],
  [-298, -770, -124],
  [-338, -772, 95],
  [-325, -818, -179],
  [-329, -780, -153],
  [-280, -796, -151],
  [-230, -755, -100],
  [-234, -759, 44],
  [-248, -807, 90],
  [-217, -872, 79],
  [-204, -887, 74],
  [-189, -939, 78],
  [-220, -1014, -129],
  [-147, -1107, -129],
  [-274, -1013, -158],
  [-301, -1007, -258],
  [-351, -1131, -346],
  [-118, -1086, -355],
  [-290, -716, -213],
  [-288, -720, -290],
  [-235, -825, -344],
  [-179, -819, -243],
  [-228, -670, -185],
  [-125, -790, -145],
  [-145, -795, -207],
  [-152, -809, 76],
  [-98, -871, -115],
  [-89, -855, -111],
  [-116, -879, 84],
  [-161, -945, -172],
  [-147, -1017, -173],
  [-278, -1012, -146],
  [-268, -1049, -247],
  [-279, -1026, -260],
  [-286, -958, -187],
  [-288, -890, -167],
  [-359, -873, -168],
  [-324, -904, -147],
  [-263, -804, -134],
  [-214, -712, 37],
  [-189, -698, 29],
  [-183, -755, 74],
  [-182, -841, 98],
  [-115, -894, 73],
  [-149, -857, 57],
  [-93, -927, -68],
  [-145, -988, -120],
  [-112, -1095, -112],
  [-201, -1059, -146],
  [-278, -1104, -206],
  [-284, -1204, -213],
  [-214, -966, -254],
  [-272, -730, -140],
  [-233, -785, -252],
  [-259, -813, -272],
  [-156, -840, -205],
  [-163, -765, -110],
  [-165, -741, 97],
  [-164, -791, 86],
  [-99, -849, -69],
  [-99, -820, -81],
  [-94, -842, -37],
  [-142, -881, -109],
  [-153, -978, -155],
  [-212, -934, 71],
  [-341, -947, 99],
  [-406, -1039, -283],
  [-265, -1146, -206],
  [-296, -979, -163],
  [-345, -864, 98],
  [-216, -907, 38],
  [-242, -809, 47],
  [-154, -736, 52],
  [-137, -700, -101],
  [-184, -743, -136],
  [-191, -850, 86],
  [-206, -883, 85],
  [-194, -875, 48],
  [-148, -937, 46],
  [-193, -983, 31],
  [-176, -1062, 43],
  [-251, -1006, -114],
  [-284, -1036, -192],
  [-374, -1181, -248],
  [-167, -1177, -271],
  [-253, -794, -128],
  [-285, -651, -129],
  [-228, -757, -227],
  [-260, -843, -201],
  [-189, -899, -253],
  [-212, -800, -136],
  [-218, -728, -136],
  [-177, -761, -129],
  [-165, -806, -137],
  [-157, -839, -122],
  [-116, -899, -104],
  [-191, -874, 77],
  [-174, -911, 95],
  [-193, -971, -147],
  [-255, -961, -127],
  [-222, -1052, -124],
  [-333, -1021, -223],
  [-245, -1018, -215],
  [-269, -850, 91],
  [-318, -754, -120],
  [-335, -878, -199],
  [-322, -986, -224],
  [-192, -902, -179],
  [-177, -712, 86],
  [-196, -673, 88],
  [-178, -751, -101],
  [-182, -847, 70],
  [-147, -909, -131],
  [-170, -939, 43],
  [-224, -994, 60],
  [-189, -1051, 42],
  [-242, -968, -183],
  [-312, -978, -213],
  [-317, -1298, -334],
  [-184, -1131, -330],
  [-287, -754, -141],
  [-249, -773, -287],
  [-166, -842, -297],
  [-196, -742, -214],
  [-163, -729, -198],
  [-177, -757, -197],
  [-174, -830, -155],
  [-159, -860, -149],
  [-145, -856, 72],
  [-132, -849, 47],
  [-145, -839, 62],
  [-179, -843, 76],
  [-163, -941, -114],
  [-230, -963, -110],
]


##################################################################################################
def real_value_fft(x):
    """ Real value FFT as described in Appendix of:
    "Real-valued Fast Fourier Transform Algorithm", from IEEE Transactions on Acoustics, Speech,
    and Signal Processing, Vol. ASSP-35, No. 6, June 1987
    """

    # Make sure we have a power of 2 length input
    n = len(x)
    m = int(math.log(n, 2))
    if (math.pow(2, m) != n):
        raise RuntimeError("Length must be a power of 2")

    # The rest of the code assumes 1-based indexing (it comes from fortran)
    x = [0] + x

    # ---------------------------------------------------------------------------------
    # Digit reverse counter
    j = 1
    n1 = n - 1
    for i in range(1, n1 + 1):
        if (i < j):
            xt = x[j]
            x[j] = x[i]
            x[i] = xt
        k = n/2
        while (k < j):
            j = j - k
            k = k / 2
        j = j + k

    # ---------------------------------------------------------------------------------
    # Length 2 butterflies
    for i in range(1, n + 1, 2):
        xt = x[i]
        x[i] = xt + x[i+1]
        x[i+1] = xt - x[i+1]

    # ---------------------------------------------------------------------------------
    # Other butterflies
    n2 = 1
    for k in range(2, m + 1):
        n4 = n2
        n2 = 2 * n4
        n1 = 2 * n2
        e = 2 * math.pi / n1
        for i in range(1, n+1, n1):
            xt = x[i]
            x[i] = xt + x[i + n2]
            x[i + n2] = xt - x[i + n2]
            x[i + n4 + n2] = -x[i + n4 + n2]

            a = e
            for j in range(1, n4 - 1):
                i1 = i + j
                i2 = i - j + n2
                i3 = i + j + n2
                i4 = i - j + n1
                cc = math.cos(a)
                ss = math.sin(a)
                a = a + e
                t1 = x[i3] * cc + x[i4] * ss
                t2 = x[i3] * ss - x[i4] * cc
                x[i4] = x[i2] - t2
                x[i3] = -x[i2] - t2
                x[i2] = x[i1] - t1
                x[i1] = x[i1] + t1

    return x[1:]


###################################################################################################
def compute_magnitude(x):
    """ The real_value_fft() produces an array containing outputs in this order:
        [Re(0), Re(1), ..., Re(N/2), Im(N/2-1), ..., Im(1)]

        This method returns the magnitudes. The magnitude of term i is sqrt(Re(i)**2 + Im(i)**2)
    """
    result = []
    n = len(x)
    real_idx = 0
    im_idx = n - 1

    result.append(x[real_idx])
    real_idx += 1

    while real_idx <= n/2 - 1:
        mag = (x[real_idx]**2 + x[im_idx]**2) ** 0.5
        result.append(mag)
        real_idx += 1
        im_idx -= 1

    result.append(x[real_idx])
    return result


###################################################################################################
def apply_gaussian(x, width=0.1):
    """ Multiply x by the gaussian function. Width is a fraction, like 0.1
    """
    result = []
    n = len(x)
    mid = float(n/2)
    denominator = n**2 * width

    for i in range(len(x)):
        print i-mid, (i-mid)**2,  -1 * (i - mid)**2/denominator, \
              math.exp(-1 * (i - mid)**2/denominator)
        g = math.exp(-1 * (i - mid)**2/denominator)
        result.append(g * x[i])

    return result


###################################################################################################
def print_graph(x):
    min_value = min(x)
    max_value = max(x)

    extent = max(abs(min_value), abs(max_value))
    scale = 2 * extent
    min_value = -extent

    for i in range(len(x)):
        print "%4d:  %10.3f: " % (i, x[i]),
        position = int((x[i] - min_value) * 80 / scale)
        if position < 40:
            print ' ' * position,
            print '*' * (40 - position)
        else:
            print ' ' * 40,
            print '*' * (position - 40)


###################################################################################################
if __name__ == '__main__':

    # Collect our command line arguments
    parser = argparse.ArgumentParser()
    parser.add_argument('--debug', action='store_true', help="Turn on debug logging")
    args = parser.parse_args()

    level = logging.INFO
    if args.debug:
        level = logging.DEBUG
    logging.basicConfig(level=level)

    # -------------------------------------------------------------------------------------
    # Constant signal
    if 0:
        input_len = 128
        input = [1 for x in range(input_len)]
        print "\n############ INPUT ######################"
        print_graph(input)

        result = real_value_fft(input)

        print "\n############ RESULT ######################"
        print_graph(result)

    # -------------------------------------------------------------------------------------
    # N sine waves
    if 0:
        input_len = 128
        freq = 7
        input = [math.cos(float(x)/input_len * freq * 2 * math.pi) for x in range(input_len)]
        print "\n############ INPUT ######################"
        print_graph(input)

        print "\n############ GAUSSIAN OF INPUT ############"
        # input = apply_gaussian(input, 0.1)
        print_graph(input)

        result = real_value_fft(input)

        print "\n############ REAL, IMAG ######################"
        print_graph(result)

        print "\n############ MAGNITUDE ######################"
        mag = compute_magnitude(result)
        print_graph(mag)

    # -------------------------------------------------------------------------------------
    # Step data
    if 1:
        input_len = 128
        raw_input = g_walk_10_steps[0:input_len]

        x_data = [x for x, y, z in raw_input]
        x_mean = sum(x_data) / len(x_data)
        x_data = [x - x_mean for x in x_data]

        y_data = [y for x, y, z in raw_input]
        y_mean = sum(y_data) / len(y_data)
        y_data = [y - y_mean for y in y_data]

        z_data = [z for x, y, z in raw_input]
        z_mean = sum(z_data) / len(z_data)
        z_data = [z - z_mean for z in z_data]

        print "\n############ X ######################"
        print_graph(x_data)
        print "\n############ Y ######################"
        print_graph(y_data)
        print "\n############ Z ######################"
        print_graph(z_data)

        input = []
        for (x, y, z) in raw_input:
            mag = x**2 + y**2 + z**2
            mag = mag ** 0.5
            input.append(mag)

        mean_mag = sum(input) / len(input)
        input = [x - mean_mag for x in input]

        print "\n############ INPUT ######################"
        # input = apply_gaussian(input)
        print_graph(input)

        result = real_value_fft(input)

        print "\n############ REAL, IMAG ######################"
        print_graph(result)

        print "\n############ MAGNITUDE ######################"
        mag = compute_magnitude(result)
        print_graph(mag)
