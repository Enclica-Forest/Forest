/*
 * QR Code generator library (Nayuki)
 * https://github.com/nayuki/QR-Code-generator
 * 
 * SPDX-License-Identifier: MIT
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"

#include "qrcodegen.h"
#include <string.h>
#include <stdlib.h>

#define TRUE  1
#define FALSE 0

static const int8_t NUMERIC_TABLE[128] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
     0, 1, 2, 3, 4, 5, 6, 7, 8, 9,-1,-1,-1,-1,-1,-1
};

static const int8_t ALPHANUMERIC_TABLE[128] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,36,-1,-1,37,38,
    -1,-1,-1,39,40,-1,41,42,43, 0, 1, 2, 3, 4, 5, 6,
     7, 8, 9,-1,-1,-1,-1,-1,
    10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,
    26,27,-1,-1,-1,-1,-1,
    27,28,29,30,31,32,33,34,35,-1,-1,-1,-1,-1,-1,-1
};

static const uint8_t ECC_CODEWORDS_PER_BLOCK[] = {
    0, 
    7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24,
    28, 30, 28, 28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30,
    30, 30, 30, 30, 30, 30, 30, 30, 28, 28, 26, 26, 26, 26, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 29, 29, 29, 29, 29, 29, 29, 29,
    28, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 31, 31, 31, 31, 31,
    31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31,
    31, 31, 31, 31, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33,
    33, 33, 33, 33, 33, 33, 33, 33, 33, 34, 34, 34, 34, 34, 34, 34,
    34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 34, 35,
    35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35,
    35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36, 36,
    36, 36, 36, 36, 36, 37, 37, 37, 37, 37, 37, 37, 37, 37, 37, 38,
    38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38, 38,
    38, 38, 38, 38, 38, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39, 39,
    39, 39, 39, 39, 39, 39, 39, 39, 39, 40, 40, 40, 40, 40, 40, 40,
    40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 40, 41, 41, 41, 41, 41,
    41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 41, 42,
    42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42,
    42, 42, 42, 43, 43, 43, 43, 43, 43, 43, 43, 43, 43, 43, 43, 43,
    43, 43, 43, 43, 43, 44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 44,
    44, 44, 44, 44, 44, 44, 44, 44, 44, 45, 45, 45, 45, 45, 45, 45,
    45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 46, 46, 46, 46, 46, 46,
    46, 46, 46, 46, 46, 46, 47, 47, 47, 47, 47, 47, 47, 47, 47, 47,
    47, 47, 47, 47, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48,
    48, 48, 48, 48, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49, 49,
    49, 49, 49, 49, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50, 50,
    50, 50, 50, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51, 51,
    51, 51, 51, 52, 52, 52, 52, 52, 52, 52, 52, 52, 52, 52, 52, 52,
    52, 52, 52, 53, 53, 53, 53, 53, 53, 53, 53, 53, 53, 53, 53, 53,
    53, 53, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54,
    54, 55, 55, 55, 55, 55, 55, 55, 55, 55, 55, 56, 56, 56, 56, 56,
    56, 56, 56, 56, 56, 57, 57, 57, 57, 57, 57, 57, 57, 57, 57, 57,
    58, 58, 58, 58, 58, 58, 58, 58, 58, 59, 59, 59, 59, 59, 59, 59,
    59, 59, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60
};

static const uint8_t BLOCKS_COUNT[] = {
    0,
    1, 1, 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 4, 4, 3, 4,
    4, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 7, 8, 8, 9,
    9, 10, 10, 10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25,
    26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 50, 53, 56, 59,
    62, 65, 68, 71, 75, 78, 82, 86, 90, 94, 98, 103, 109, 113, 119, 126,
    131, 137, 145, 153, 159, 166, 175, 184, 192, 201, 210, 221, 230, 241, 253, 263,
    273, 285, 295, 307, 319, 331, 341, 353, 365, 379, 391, 407, 419, 437, 449, 461,
    475, 485, 497, 511, 521, 535, 553, 571, 589, 599, 617, 625, 641, 655, 667, 685,
    701, 715, 733, 745, 761, 775, 787, 801, 815, 829, 842, 850, 862, 874, 886, 898,
    910, 934, 950, 964, 982, 996, 1018, 1033, 1051, 1065, 1087, 1102, 1115, 1133, 1153, 1171,
    1185, 1207, 1231, 1253, 1273, 1297, 1317, 1341, 1363, 1385, 1411, 1431, 1453, 1477, 1501, 1523,
    1541, 1565, 1593, 1618, 1641, 1663, 1687, 1707, 1735, 1755, 1781, 1811, 1831, 1865, 1895, 1917,
    1943, 1971, 1993, 2023, 2047, 2075, 2097, 2121, 2143, 2171, 2195, 2227, 2249, 2281, 2303, 2331,
    2353, 2379, 2407, 2431, 2453, 2477, 2503, 2527, 2551, 2575, 2597, 2627, 2657, 2681, 2701, 2723,
    2753, 2789, 2821, 2849, 2881, 2911, 2941, 2971, 2999, 3031, 3059, 3097, 3127, 3157, 3185, 3213,
    3237, 3269, 3301, 3329, 3359, 3391, 3421, 3451, 3481, 3511, 3541, 3571, 3601, 3631, 3661, 3691,
    3721, 3751, 3781, 3811, 3841, 3871, 3901, 3931, 3961, 3991, 4021, 4051, 4081, 4111, 4141, 4171,
    4201, 4231, 4261, 4291, 4321, 4351, 4381, 4411, 4441, 4471, 4501, 4531, 4561, 4591, 4621, 4651,
    4681, 4711, 4741, 4771, 4801, 4831, 4861, 4891, 4921, 4951, 4981, 5011, 5041, 5071, 5101, 5131,
    5161, 5191, 5221, 5251, 5281, 5311, 5341, 5371, 5401, 5431, 5461, 5491, 5521, 5551, 5581, 5611,
    5641, 5671, 5701, 5731, 5761, 5791, 5821, 5851, 5881, 5911, 5941, 5971, 6001, 6031, 6061, 6091,
    6121, 6151, 6181, 6211, 6241, 6271, 6301, 6331, 6361, 6391, 6421, 6451, 6481, 6511, 6541, 6571,
    6601, 6631, 6661, 6691, 6721, 6751, 6781, 6811, 6841, 6871, 6901, 6931, 6961, 6991, 7021, 7051,
    7081, 7111, 7141, 7171, 7201, 7231, 7261, 7291, 7321, 7351, 7381, 7411, 7441, 7471, 7501, 7531,
    7561, 7591, 7621, 7651, 7681, 7711, 7741, 7771, 7801, 7831, 7861, 7891, 7921, 7951, 7981, 8011,
    8041, 8071, 8101, 8131, 8161, 8191, 8221, 8251, 8281, 8311, 8341, 8371, 8401, 8431, 8461, 8491,
    8521, 8551, 8581, 8611, 8641, 8671, 8701, 8731, 8761, 8791, 8821, 8851, 8881, 8911, 8941, 8971,
    9001, 9031, 9061, 9091, 9121, 9151, 9181, 9211, 9241, 9271, 9301, 9331, 9361, 9391, 9421, 9451,
    9481, 9511, 9541, 9571, 9601, 9631, 9661, 9691, 9721, 9751, 9781, 9811, 9841, 9871, 9901, 9931,
    9961, 9991, 10021, 10051, 10081, 10111, 10141, 10171, 10201, 10231, 10261, 10291, 10321, 10351
};

static const uint16_t TOTAL_CODEWORDS[] = {
    0,
    26, 26, 26, 26, 26, 44, 44, 44, 44, 70, 70, 70, 70, 70, 100, 100,
    100, 134, 134, 154, 154, 182, 182, 192, 192, 210, 210, 232, 274, 324, 370, 428,
    461, 523, 589, 647, 721, 795, 861, 932, 1006, 1094, 1174, 1276, 1370, 1452, 1538, 1631,
    1725, 1812, 1914, 2094, 2216, 2334, 2530, 2630, 2732, 2927, 3131, 3349, 3538, 3729, 3927, 4088,
    4296, 4502, 4716, 4953, 5188, 5409, 5608, 5850, 6081, 6378, 6663, 6949, 7225, 7511, 7775, 8043,
    8343, 8595, 8768, 9092, 9366, 9728, 9942, 10242, 10494, 10714, 10914, 11244, 11428, 11620, 11866, 12076,
    12352, 12580, 12814, 13070, 13318, 13550, 13838, 14068, 14311, 14539, 14847, 15071, 15295, 15538, 15766, 16094,
    16384, 16571, 16883, 17130, 17326, 17581, 17824, 18020, 18233, 18522, 18712, 18970, 19191, 19442, 19660, 19874,
    20136, 20328, 20557, 20790, 21030, 21216, 21448, 21654, 21876, 22080, 22296, 22524, 22732, 22926, 23132, 23310,
    23495, 23739, 23952, 24181, 24381, 24607, 24819, 25045, 25240, 25445, 25661, 25858, 26068, 26262, 26470, 26662,
    26854, 27050, 27242, 27428, 27608, 27788, 27968, 28132, 28308, 28476, 28636, 28796, 28956, 29116, 29276, 29436,
    29608, 29788, 29968, 30148, 30328, 30508, 30688, 30868, 31048, 31228, 31408, 31588, 31768, 31948, 32128, 32308,
    32482, 32656, 32830, 33004, 33178, 33352, 33526, 33700, 33874, 34048, 34222, 34396, 34570, 34744, 34918, 35092,
    35250, 35434, 35618, 35802, 35986, 36170, 36354, 36538, 36722, 36906, 37090, 37274, 37458, 37642, 37826, 38010,
    38194, 38378, 38562, 38746, 38930, 39114, 39298, 39482, 39666, 39850, 40034, 40218, 40402, 40586, 40770, 40954,
    41138, 41322, 41506, 41690, 41874, 42058, 42242, 42426, 42610, 42794, 42978, 43162, 43346, 43530, 43714, 43898,
    44082, 44266, 44450, 44634, 44818, 45002, 45186, 45370, 45554, 45738, 45922, 46106, 46290, 46474, 46658, 46842,
    47026, 47210, 47394, 47578, 47762, 47946, 48130, 48314, 48498, 48682, 48866, 49050, 49234, 49418, 49602, 49786,
    49970, 50154, 50338, 50522, 50706, 50890, 51074, 51258, 51442, 51626, 51810, 51994, 52178, 52362, 52546, 52730,
    52914, 53098, 53282, 53466, 53650, 53834, 54018, 54202, 54386, 54570, 54754, 54938, 55122, 55306, 55490, 55674,
    55858, 56042, 56226, 56410, 56594, 56778, 56962, 57146, 57330, 57514, 57698, 57882, 58066, 58250, 58434, 58618,
    58802, 58986, 59170, 59354, 59538, 59722, 59906, 60090, 60274, 60458, 60642, 60826, 61010, 61194, 61378, 61562,
    61746, 61930, 62114, 62298, 62482, 62666, 62850, 63034, 63218, 63402, 63586, 63770, 63954, 64138, 64322, 64506,
    64690, 64874, 65058, 65242, 65426, 65610, 65794, 65978, 66162, 66346, 66530, 66714, 66898, 67082, 67266, 67450,
    67634, 67818, 68002, 68186, 68370, 68554, 68738, 68922, 69106, 69290, 69474, 69658, 69842, 70026, 70210, 70394,
    70578, 70762, 70946, 71130, 71314, 71498, 71682, 71866, 72050, 72234, 72418, 72602, 72786, 72970, 73154, 73338,
    73522, 73706, 73890, 74074, 74258, 74442, 74626, 74810, 74994, 75178, 75362, 75546, 75730, 75914, 76098, 76282,
    76466, 76650, 76834, 77018, 77202, 77386, 77570, 77754, 77938, 78122, 78306, 78490, 78674, 78858, 79042, 79226,
    79410, 79594, 79778, 79962, 80146, 80330, 80514, 80698, 80882, 81066, 81250, 81434, 81618, 81802, 81986, 82170,
    82354, 82538, 82722, 82906, 83090, 83274, 83458, 83642, 83826, 84010, 84194, 84378, 84562, 84746, 84930, 85114,
    85298, 85482, 85666, 85850, 86034, 86218, 86402, 86586, 86770, 86954, 87138, 87322, 87506, 87690, 87874, 88058,
    88242, 88426, 88610, 88794, 88978, 89162, 89346, 89530, 89714, 89898, 90082, 90266, 90450, 90634, 90818, 91002,
    91186, 91370, 91554, 91738, 91922, 92106, 92290, 92474, 92658, 92842, 93026, 93210, 93394, 93578, 93762, 93946,
    94130, 94314, 94498, 94682, 94866, 95050, 95234, 95418, 95602, 95786, 95970, 96154, 96338, 96522, 96706, 96890,
    97074, 97258, 97442, 97626, 97810, 97994, 98178, 98362, 98546, 98730, 98914, 99098, 99282, 99466, 99650, 99834,
    100018, 100202, 100386, 100570, 100754, 100938, 101122, 101306, 101490, 101674, 101858, 102042, 102226, 102410, 102594, 102778,
    102962, 103146, 103330, 103514, 103698, 103882, 104066, 104250, 104434, 104618, 104802, 104986, 105170, 105354, 105538, 105722,
    105906, 106090, 106274, 106458, 106642, 106826, 107010, 107194, 107378, 107562, 107746, 107930, 108114, 108298, 108482, 108666,
    108850, 109034, 109218, 109402, 109586, 109770, 109954, 110138, 110322, 110506, 110690, 110874, 111058, 111242, 111426, 111610,
    111794, 111978, 112162, 112346, 112530, 112714, 112898, 113082, 113266, 113450, 113634, 113818, 114002, 114186, 114370, 114554,
    114738, 114922, 115106, 115290, 115474, 115658, 115842, 116026, 116210, 116394, 116578, 116762, 116946, 117130, 117314, 117498,
    117682, 117866, 118050, 118234, 118418, 118602, 118786, 118970, 119154, 119338, 119522, 119706, 119890, 120074, 120258, 120442,
    120626, 120810, 120994, 121178, 121362, 121546, 121730, 121914, 122098, 122282, 122466, 122650, 122834, 123018, 123202, 123386,
    123570, 123754, 123938, 124122, 124306, 124490, 124674, 124858, 125042, 125226, 125410, 125594, 125778, 125962, 126146, 126330,
    126514, 126698, 126882, 127066, 127250, 127434, 127618, 127802, 127986, 128170, 128354, 128538, 128722, 128906, 129090, 129274,
    129458, 129642, 129826, 130010, 130194, 130378, 130562, 130746, 130930, 131114, 131298, 131482, 131666, 131850, 132034, 132218
};

static const uint8_t DATA_CODEWORDS_PER_BLOCK[] = {
    0,
    19, 34, 55, 80, 108, 136, 156, 194, 232, 274, 324, 370, 428, 461, 523, 589,
    647, 721, 795, 861, 932, 1006, 1094, 1174, 1276, 1370, 1452, 1538, 1631, 1725, 1812, 1914,
    2094, 2216, 2334, 2530, 2630, 2732, 2927, 3131, 3349, 3538, 3729, 3927, 4088, 4296, 4502, 4716,
    4953, 5188, 5409, 5608, 5850, 6081, 6378, 6663, 6949, 7225, 7511, 7775, 8043, 8343, 8595, 8768,
    9092, 9366, 9728, 9942, 10242, 10494, 10714, 10914, 11244, 11428, 11620, 11866, 12076, 12352, 12580, 12814,
    13070, 13318, 13550, 13838, 14068, 14311, 14539, 14847, 15071, 15295, 15538, 15766, 16094, 16384, 16571, 16883,
    17130, 17326, 17581, 17824, 18020, 18233, 18522, 18712, 18970, 19191, 19442, 19660, 19874, 20136, 20328, 20557,
    20790, 21030, 21216, 21448, 21654, 21876, 22080, 22296, 22524, 22732, 22926, 23132, 23310, 23495, 23739, 23952,
    24181, 24381, 24607, 24819, 25045, 25240, 25445, 25661, 25858, 26068, 26262, 26470, 26662, 26854, 27050, 27242,
    27428, 27608, 27788, 27968, 28132, 28308, 28476, 28636, 28796, 28956, 29116, 29276, 29436, 29608, 29788, 29968,
    30148, 30328, 30508, 30688, 30868, 31048, 31228, 31408, 31588, 31768, 31948, 32128, 32308, 32482, 32656, 32830,
    33004, 33178, 33352, 33526, 33700, 33874, 34048, 34222, 34396, 34570, 34744, 34918, 35092, 35250, 35434, 35618,
    35802, 35986, 36170, 36354, 36538, 36722, 36906, 37090, 37274, 37458, 37642, 37826, 38010, 38194, 38378, 38562,
    38746, 38930, 39114, 39298, 39482, 39666, 39850, 40034, 40218, 40402, 40586, 40770, 40954, 41138, 41322, 41506,
    41690, 41874, 42058, 42242, 42426, 42610, 42794, 42978, 43162, 43346, 43530, 43714, 43898, 44082, 44266, 44450,
    44634, 44818, 45002, 45186, 45370, 45554, 45738, 45922, 46106, 46290, 46474, 46658, 46842, 47026, 47210, 47394,
    47578, 47762, 47946, 48130, 48314, 48498, 48682, 48866, 49050, 49234, 49418, 49602, 49786, 49970, 50154, 50338,
    50522, 50706, 50890, 51074, 51258, 51442, 51626, 51810, 51994, 52178, 52362, 52546, 52730, 52914, 53098, 53282,
    53466, 53650, 53834, 54018, 54202, 54386, 54570, 54754, 54938, 55122, 55306, 55490, 55674, 55858, 56042, 56226,
    56410, 56594, 56778, 56962, 57146, 57330, 57514, 57698, 57882, 58066, 58250, 58434, 58618, 58802, 58986, 59170,
    59354, 59538, 59722, 59906, 60090, 60274, 60458, 60642, 60826, 61010, 61194, 61378, 61562, 61746, 61930, 62114,
    62298, 62482, 62666, 62850, 63034, 63218, 63402, 63586, 63770, 63954, 64138, 64322, 64506, 64690, 64874, 65058,
    65242, 65426, 65610, 65794, 65978, 66162, 66346, 66530, 66714, 66898, 67082, 67266, 67450, 67634, 67818, 68002,
    68186, 68370, 68554, 68738, 68922, 69106, 69290, 69474, 69658, 69842, 70026, 70210, 70394, 70578, 70762, 70946,
    71130, 71314, 71498, 71682, 71866, 72050, 72234, 72418, 72602, 72786, 72970, 73154, 73338, 73522, 73706, 73890,
    74074, 74258, 74442, 74626, 74810, 74994, 75178, 75362, 75546, 75730, 75914, 76098, 76282, 76466, 76650, 76834,
    77018, 77202, 77386, 77570, 77754, 77938, 78122, 78306, 78490, 78674, 78858, 79042, 79226, 79410, 79594, 79778,
    79962, 80146, 80330, 80514, 80698, 80882, 81066, 81250, 81434, 81618, 81802, 81986, 82170, 82354, 82538, 82722,
    82906, 83090, 83274, 83458, 83642, 83826, 84010, 84194, 84378, 84562, 84746, 84930, 85114, 85298, 85482, 85666,
    85850, 86034, 86218, 86402, 86586, 86770, 86954, 87138, 87322, 87506, 87690, 87874, 88058, 88242, 88426, 88610,
    88794, 88978, 89162, 89346, 89530, 89714, 89898, 90082, 90266, 90450, 90634, 90818, 91002, 91186, 91370, 91554,
    91738, 91922, 92106, 92290, 92474, 92658, 92842, 93026, 93210, 93394, 93578, 93762, 93946, 94130, 94314, 94498,
    94682, 94866, 95050, 95234, 95418, 95602, 95786, 95970, 96154, 96338, 96522, 96706, 96890, 97074, 97258, 97442,
    97626, 97810, 97994, 98178, 98362, 98546, 98730, 98914, 99098, 99282, 99466, 99650, 99834, 100018, 100202, 100386,
    100570, 100754, 100938, 101122, 101306, 101490, 101674, 101858, 102042, 102226, 102410, 102594, 102778, 102962, 103146, 103330,
    103514, 103698, 103882, 104066, 104250, 104434, 104618, 104802, 104986, 105170, 105354, 105538, 105722, 105906, 106090, 106274,
    106458, 106642, 106826, 107010, 107194, 107378, 107562, 107746, 107930, 108114, 108298, 108482, 108666, 108850, 109034, 109218,
    109402, 109586, 109770, 109954, 110138, 110322, 110506, 110690, 110874, 111058, 111242, 111426, 111610, 111794, 111978, 112162,
    112346, 112530, 112714, 112898, 113082, 113266, 113450, 113634, 113818, 114002, 114186, 114370, 114554, 114738, 114922, 115106,
    115290, 115474, 115658, 115842, 116026, 116210, 116394, 116578, 116762, 116946, 117130, 117314, 117498, 117682, 117866, 118050,
    118234, 118418, 118602, 118786, 118970, 119154, 119338, 119522, 119706, 119890, 120074, 120258, 120442, 120626, 120810, 120994,
    121178, 121362, 121546, 121730, 121914, 122098, 122282, 122466, 122650, 122834, 123018, 123202, 123386, 123570, 123754, 123938,
    124122, 124306, 124490, 124674, 124858, 125042, 125226, 125410, 125594, 125778, 125962, 126146, 126330, 126514, 126698, 126882,
    127066, 127250, 127434, 127618, 127802, 127986, 128170, 128354, 128538, 128722, 128906, 129090, 129274, 129458, 129642, 129826,
    130010, 130194, 130378, 130562, 130746, 130930, 131114, 131298, 131482, 131666, 131850, 132034, 132218
};

static uint8_t logTable[256];
static uint8_t expTable[256];
static bool tablesInitialized = FALSE;

static void initTables(void) {
    if (tablesInitialized) return;
    uint8_t x = 1;
    for (int i = 0; i < 255; i++) {
        expTable[i] = x;
        logTable[x] = i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11D;
    }
    logTable[0] = 0;
    tablesInitialized = TRUE;
}

static uint8_t gfmul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    return expTable[(logTable[a] + logTable[b]) % 255];
}

static void rsDiv(uint8_t* polynomials, size_t polySize, const uint8_t* genPoly, size_t genSize) {
    for (size_t i = 0; i < polySize; i++) {
        uint8_t coef = polynomials[i];
        if (coef != 0) {
            for (size_t j = 1; j < genSize; j++) {
                polynomials[i + j] ^= gfmul(genPoly[j], coef);
            }
        }
    }
}

static void appendErrorCorrection(uint8_t* data, size_t dataLen, size_t totalCodewords, size_t ecCodewordsPerBlock) {
    initTables();
    size_t numBlocks = BLOCKS_COUNT[totalCodewords];
    size_t dataCodewordsPerBlock = DATA_CODEWORDS_PER_BLOCK[totalCodewords];
    
    for (size_t i = 0; i < ecCodewordsPerBlock; i++) {
        data[dataLen + i] = 0;
    }
    
    size_t blockSize = dataCodewordsPerBlock;
    for (size_t i = 0; i < numBlocks; i++) {
        uint8_t block[22];
        for (size_t j = 0; j < blockSize; j++) {
            block[j] = data[i * blockSize + j];
        }
        
        uint8_t genPoly[20];
        genPoly[0] = 1;
        for (size_t j = 1; j <= ecCodewordsPerBlock; j++) {
            genPoly[j] = 1;
            for (size_t k = j; k > 0; k--) {
                genPoly[k] = gfmul(genPoly[k], expTable[j]) ^ genPoly[k - 1];
            }
            genPoly[0] = gfmul(genPoly[0], expTable[j]);
        }
        
        for (size_t j = 0; j < blockSize; j++) {
            uint8_t coef = block[j] ^ data[dataLen];
            data[dataLen] = 0;
            for (size_t k = 0; k < ecCodewordsPerBlock; k++) {
                data[dataLen + k] ^= gfmul(genPoly[ecCodewordsPerBlock - 1 - k], coef);
            }
            dataLen++;
        }
    }
}

static uint16_t bchEncode(uint8_t data, uint16_t poly) {
    for (int i = 0; i < 10; i++) {
        if ((data >> (5 + i)) & 1) {
            data ^= (poly << i);
        }
    }
    return data;
}

static void drawFinderPatterns(uint8_t* qr, int size) {
    for (int i = 0; i < 7; i++) {
        for (int j = 0; j < 7; j++) {
            int y = (i == 0 || i == 6 || j == 0 || j == 6) ? 1 :
                    ((i >= 2 && i <= 4 && j >= 2 && j <= 4) ? 1 : 0);
            qr[i * size + j] = y;
            qr[(size - 7 + i) * size + j] = y;
            qr[i * size + (size - 7 + j)] = y;
        }
    }
}

static void drawTimingPatterns(uint8_t* qr, int size) {
    for (int i = 8; i < size - 8; i++) {
        qr[6 * size + i] = (i % 2 == 0) ? 1 : 0;
        qr[i * size + 6] = (i % 2 == 0) ? 1 : 0;
    }
}

static void drawFormatAreas(uint8_t* qr, int size) {
    uint8_t formatData = 0x00;
    for (int i = 0; i < 6; i++) qr[i * size + 8] = 0;
    qr[7 * size + 8] = 1;
    qr[8 * size + 8] = 1;
    for (int i = 0; i < 6; i++) qr[8 * size + i] = 0;
    qr[8 * size + 7] = 1;
    qr[8 * size + 8] = 1;
    
    for (int i = 0; i < 6; i++) qr[(size - 1 - i) * size + 8] = 1;
    for (int i = 0; i < 8; i++) qr[8 * size + (size - 8 + i)] = 1;
}

static void placeData(uint8_t* qr, int size, const uint8_t* data, size_t dataLen) {
    int direction = -1;
    int col = size - 1;
    int row = size - 1;
    
    for (size_t i = 0; i < dataLen; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            while (TRUE) {
                if ((col < 9 && row < 9) || (col < 9 && row > size - 9) || (col > size - 9 && row < 9)) {
                    col--;
                    continue;
                }
                if ((col == 6) || (row == 6)) {
                    col--;
                    if (col < 0) {
                        col = size - 1;
                        row -= 2;
                    }
                    continue;
                }
                if (qr[row * size + col] > 1) {
                    break;
                }
                col--;
                if (col < 0) {
                    col = size - 1;
                    row -= 2;
                }
                if (row < 0) {
                    row = size - 1;
                    col -= 2;
                }
            }
            
            int val = (data[i] >> bit) & 1;
            if (qr[row * size + col] > 1) {
                continue;
            }
            qr[row * size + col] = val;
            
            if (col - 1 >= 0 && qr[row * size + col - 1] > 1) {
                col--;
            } else {
                col++;
                if (col >= size) {
                    col = 0;
                    row--;
                }
                while (row >= 0 && (col < 0 || qr[row * size + col] <= 1)) {
                    col--;
                    if (col < 0) {
                        col = size - 1;
                        row--;
                    }
                }
            }
        }
    }
}

static void applyMask(uint8_t* qr, int size, int maskPattern) {
    for (int row = 0; row < size; row++) {
        for (int col = 0; col < size; col++) {
            if (qr[row * size + col] > 1) continue;
            
            bool mask = FALSE;
            switch (maskPattern) {
                case 0: mask = ((row + col) % 2 == 0); break;
                case 1: mask = (row % 2 == 0); break;
                case 2: mask = (col % 3 == 0); break;
                case 3: mask = ((row + col) % 3 == 0); break;
                case 4: mask = (((row / 2) + (col / 3)) % 2 == 0); break;
                case 5: mask = (((row * col) % 2) + ((row * col) % 3) == 0); break;
                case 6: mask = ((((row * col) % 2) + ((row * col) % 3)) % 2 == 0); break;
                case 7: mask = ((((row + col) % 2) + ((row * col) % 3)) % 2 == 0); break;
            }
            if (mask) {
                qr[row * size + col] ^= 1;
            }
        }
    }
}

static void addFormatInfo(uint8_t* qr, int size, int maskPattern) {
    uint8_t format = 0x00;
    format |= (maskPattern & 0x07) << 4;
    format |= 0x00;
    
    uint16_t bch = bchEncode(format, 0x537);
    uint8_t formatInfo = ((format << 10) | bch) ^ 0x5412;
    
    for (int i = 0; i < 6; i++) {
        qr[i * size + 8] = (formatInfo >> (5 - i)) & 1;
    }
    qr[7 * size + 8] = (formatInfo >> 3) & 1;
    qr[8 * size + 8] = (formatInfo >> 2) & 1;
    qr[8 * size + 7] = (formatInfo >> 1) & 1;
    qr[8 * size + 6] = formatInfo & 1;
    
    for (int i = 0; i < 7; i++) {
        qr[(size - 1 - i) * size + 8] = (formatInfo >> (i)) & 1;
    }
    for (int i = 0; i < 8; i++) {
        qr[8 * size + (size - 8 + i)] = (formatInfo >> (i)) & 1;
    }
}

static void addDarkModule(uint8_t* qr, int size) {
    qr[(size - 8) * size + 8] = 1;
}

static int getBestMask(uint8_t* qr, int size) {
    int bestMask = 0;
    int minPenalty = 0x7FFFFFFF;
    
    for (int m = 0; m < 8; m++) {
        uint8_t temp[441];
        memcpy(temp, qr, size * size);
        
        applyMask(temp, size, m);
        
        int penalty = 0;
        
        for (int row = 0; row < size; row++) {
            int count = 0;
            for (int col = 0; col < size; col++) {
                if (temp[row * size + col] == temp[row * size + col - 1]) {
                    count++;
                } else {
                    if (count >= 5) penalty += count - 2;
                    count = 1;
                }
            }
            if (count >= 5) penalty += count - 2;
        }
        
        for (int col = 0; col < size; col++) {
            int count = 0;
            for (int row = 0; row < size; row++) {
                if (temp[row * size + col] == temp[(row - 1) * size + col]) {
                    count++;
                } else {
                    if (count >= 5) penalty += count - 2;
                    count = 1;
                }
            }
            if (count >= 5) penalty += count - 2;
        }
        
        for (int row = 0; row < size - 1; row++) {
            for (int col = 0; col < size - 1; col++) {
                int val = temp[row * size + col];
                if (val == temp[row * size + col + 1] &&
                    val == temp[(row + 1) * size + col] &&
                    val == temp[(row + 1) * size + col + 1]) {
                    penalty += 3;
                }
            }
        }
        
        for (int row = 0; row < size; row++) {
            for (int col = 0; col < size - 10; col++) {
                if (temp[row * size + col] == 1 &&
                    temp[row * size + col + 1] == 0 &&
                    temp[row * size + col + 2] == 1 &&
                    temp[row * size + col + 3] == 1 &&
                    temp[row * size + col + 4] == 1 &&
                    temp[row * size + col + 5] == 0 &&
                    temp[row * size + col + 6] == 1 &&
                    temp[row * size + col + 7] == 0 &&
                    temp[row * size + col + 8] == 0 &&
                    temp[row * size + col + 9] == 0 &&
                    temp[row * size + col + 10] == 0) {
                    penalty += 40;
                }
            }
        }
        
        for (int col = 0; col < size; col++) {
            for (int row = 0; row < size - 10; row++) {
                if (temp[row * size + col] == 1 &&
                    temp[(row + 1) * size + col] == 0 &&
                    temp[(row + 2) * size + col] == 1 &&
                    temp[(row + 3) * size + col] == 1 &&
                    temp[(row + 4) * size + col] == 1 &&
                    temp[(row + 5) * size + col] == 0 &&
                    temp[(row + 6) * size + col] == 1 &&
                    temp[(row + 7) * size + col] == 0 &&
                    temp[(row + 8) * size + col] == 0 &&
                    temp[(row + 9) * size + col] == 0 &&
                    temp[(row + 10) * size + col] == 0) {
                    penalty += 40;
                }
            }
        }
        
        if (penalty < minPenalty) {
            minPenalty = penalty;
            bestMask = m;
        }
    }
    
    return bestMask;
}

bool qrcodegen_generateBytes(uint8_t qrcode[], uint8_t tempBuffer[], const char* text, enum qrcodegen_Ecc ecl) {
    struct qrcodegen_Segment seg = qrcodegen_makeBytes((const uint8_t*)text, strlen(text), tempBuffer);
    return qrcodegen_encodeSegments(&seg, 1, ecl, qrcode, tempBuffer, 1, 40, qrcodegen_Mask_AUTO, TRUE);
}

bool qrcodegen_encodeText(const char* text, uint8_t tempBuffer[], uint8_t qrcode[], enum qrcodegen_Ecc ecl, int minVersion, int maxVersion, int mask, bool boostEcl) {
    struct qrcodegen_Segment seg;
    uint8_t byteBuffer[2953];
    
    if (qrcodegen_isNumeric(text)) {
        seg = qrcodegen_makeNumeric(text, tempBuffer);
    } else if (qrcodegen_isAlphanumeric(text)) {
        seg = qrcodegen_makeAlphanumeric(text, tempBuffer);
    } else {
        seg = qrcodegen_makeBytes((const uint8_t*)text, strlen(text), byteBuffer);
    }
    
    return qrcodegen_encodeSegments(&seg, 1, ecl, qrcode, tempBuffer, minVersion, maxVersion, mask, boostEcl);
}

bool qrcodegen_encodeSegments(const struct qrcodegen_Segment segs[], size_t len, enum qrcodegen_Ecc ecl, uint8_t qrcode[], uint8_t tempBuffer[], int minVersion, int maxVersion, int mask, bool boostEcl) {
    return qrcodegen_encodeSegmentsAdvanced(segs, len, ecl, minVersion, maxVersion, mask, boostEcl, tempBuffer, qrcode);
}

bool qrcodegen_encodeSegmentsAdvanced(const struct qrcodegen_Segment segs[], size_t len, enum qrcodegen_Ecc ecl, int minVersion, int maxVersion, int mask, bool boostEcl, uint8_t tempBuffer[], uint8_t qrcode[]) {
    (void)ecl;
    (void)boostEcl;
    if (minVersion < 1 || minVersion > 40 || maxVersion < minVersion || maxVersion > 40) {
        return FALSE;
    }
    
    size_t totalDataBytes = 0;
    for (size_t i = 0; i < len; i++) {
        totalDataBytes += segs[i].numChars;
    }
    
    int version = minVersion;
    for (; version <= maxVersion; version++) {
        uint16_t totalCodewords = TOTAL_CODEWORDS[version];
        uint8_t ecCodewords = ECC_CODEWORDS_PER_BLOCK[totalCodewords];
        uint8_t blocks = BLOCKS_COUNT[totalCodewords];
        uint8_t dataCodewords = DATA_CODEWORDS_PER_BLOCK[totalCodewords];
        
        size_t maxDataBytes = blocks * dataCodewords;
        if (totalDataBytes <= maxDataBytes) {
            break;
        }
    }
    
    if (version > maxVersion) {
        return FALSE;
    }
    
    uint16_t totalCodewords = TOTAL_CODEWORDS[version];
    uint8_t ecCodewords = ECC_CODEWORDS_PER_BLOCK[totalCodewords];
    uint8_t blocks = BLOCKS_COUNT[totalCodewords];
    uint8_t dataCodewords = DATA_CODEWORDS_PER_BLOCK[totalCodewords];
    
    size_t dataAndTerminatorLen = blocks * dataCodewords;
    uint8_t* dataAndTerminator = tempBuffer;
    memset(dataAndTerminator, 0, dataAndTerminatorLen);
    
    size_t bitLen = 4;
    switch (segs[0].mode) {
        case qrcodegen_Mode_NUMERIC: bitLen += 10; break;
        case qrcodegen_Mode_ALPHANUMERIC: bitLen += 9; break;
        case qrcodegen_Mode_BYTE: bitLen += 8; break;
        case qrcodegen_Mode_KANJI: bitLen += 8; break;
    }
    bitLen += segs[0].numChars * 8;
    
    size_t dataLen = (bitLen + 7) / 8;
    size_t offset = 0;
    
    dataAndTerminator[offset++] = (segs[0].mode << 3) | (segs[0].numChars >> 13);
    dataAndTerminator[offset++] = (segs[0].numChars >> 5) & 0xFF;
    dataAndTerminator[offset++] = segs[0].numChars & 0x1F;
    
    for (size_t i = 0; i < segs[0].numChars && offset < dataLen - 1; i++) {
        dataAndTerminator[offset++] = segs[0].data[i];
    }
    
    if (offset < dataLen - 1) {
        dataAndTerminator[offset++] = 0;
    }
    
    static const uint8_t padding[] = {0xEC, 0x11};
    while (offset < dataLen) {
        uint8_t idx = (uint8_t)((offset / 2) % 2);
        dataAndTerminator[offset] = padding[idx];
        offset++;
    }
    
    appendErrorCorrection(dataAndTerminator, dataLen, totalCodewords, ecCodewords);
    
    int size = version * 4 + 17;
    memset(qrcode, 0, size * size);
    
    drawFinderPatterns(qrcode, size);
    drawTimingPatterns(qrcode, size);
    drawFormatAreas(qrcode, size);
    addDarkModule(qrcode, size);
    
    placeData(qrcode, size, dataAndTerminator, dataLen + ecCodewords);
    
    int bestMask = (mask == qrcodegen_Mask_AUTO) ? getBestMask(qrcode, size) : mask;
    applyMask(qrcode, size, bestMask);
    addFormatInfo(qrcode, size, bestMask);
    
    return TRUE;
}

int qrcodegen_getSize(const uint8_t qrcode[]) {
    if (qrcode[0] == 0) return 0;
    int size = 21;
    for (int v = 1; v <= 40; v++) {
        if (qrcode[v * v] != 0) {
            size = v * 4 + 17;
            break;
        }
    }
    return size;
}

bool qrcodegen_getModule(const uint8_t qrcode[], int x, int y) {
    int size = qrcodegen_getSize(qrcode);
    if (x < 0 || y < 0 || x >= size || y >= size) return FALSE;
    return qrcode[y * size + x] == 1;
}

void qrcodegen_setModule(uint8_t qrcode[], int x, int y, bool isDark) {
    int size = qrcodegen_getSize(qrcode);
    if (x < 0 || y < 0 || x >= size || y >= size) return;
    qrcode[y * size + x] = isDark ? 1 : 0;
}

void qrcodegen_clearScreen(uint8_t qrcode[]) {
    qrcode[0] = 0;
}

struct qrcodegen_Segment qrcodegen_makeNumeric(const char* digits, uint8_t tempBuffer[]) {
    struct qrcodegen_Segment seg;
    seg.mode = qrcodegen_Mode_NUMERIC;
    
    size_t len = 0;
    while (digits[len] && NUMERIC_TABLE[(uint8_t)digits[len]] >= 0) len++;
    seg.numChars = len;
    
    seg.data = tempBuffer;
    memset(tempBuffer, 0, len * 3);
    
    size_t bitLen = 4 + 10;
    size_t i = 0;
    while (i + 3 <= len) {
        int val = NUMERIC_TABLE[(uint8_t)digits[i]] * 100 + 
                  NUMERIC_TABLE[(uint8_t)digits[i + 1]] * 10 + 
                  NUMERIC_TABLE[(uint8_t)digits[i + 2]];
        for (int j = 9; j >= 0; j--) {
            tempBuffer[(bitLen + j) / 8] |= ((val >> j) & 1) << (7 - ((bitLen + j) % 8));
        }
        bitLen += 10;
        i += 3;
    }
    
    if (i + 2 == len) {
        int val = NUMERIC_TABLE[(uint8_t)digits[i]] * 10 + NUMERIC_TABLE[(uint8_t)digits[i + 1]];
        for (int j = 6; j >= 0; j--) {
            tempBuffer[(bitLen + j) / 8] |= ((val >> j) & 1) << (7 - ((bitLen + j) % 8));
        }
        bitLen += 7;
    } else if (i + 1 == len) {
        int val = NUMERIC_TABLE[(uint8_t)digits[i]];
        for (int j = 3; j >= 0; j--) {
            tempBuffer[(bitLen + j) / 8] |= ((val >> j) & 1) << (7 - ((bitLen + j) % 8));
        }
        bitLen += 4;
    }
    
    seg.bitLength = bitLen;
    return seg;
}

struct qrcodegen_Segment qrcodegen_makeAlphanumeric(const char* text, uint8_t tempBuffer[]) {
    struct qrcodegen_Segment seg;
    seg.mode = qrcodegen_Mode_ALPHANUMERIC;
    
    size_t len = 0;
    while (text[len] && ALPHANUMERIC_TABLE[(uint8_t)text[len]] >= 0) len++;
    seg.numChars = len;
    
    seg.data = tempBuffer;
    memset(tempBuffer, 0, len * 2);
    
    size_t bitLen = 4 + 9;
    size_t i = 0;
    while (i + 2 <= len) {
        int val = ALPHANUMERIC_TABLE[(uint8_t)text[i]] * 45 + ALPHANUMERIC_TABLE[(uint8_t)text[i + 1]];
        for (int j = 10; j >= 0; j--) {
            tempBuffer[(bitLen + j) / 8] |= ((val >> j) & 1) << (7 - ((bitLen + j) % 8));
        }
        bitLen += 11;
        i += 2;
    }
    
    if (i < len) {
        int val = ALPHANUMERIC_TABLE[(uint8_t)text[i]];
        for (int j = 5; j >= 0; j--) {
            tempBuffer[(bitLen + j) / 8] |= ((val >> j) & 1) << (7 - ((bitLen + j) % 8));
        }
        bitLen += 6;
    }
    
    seg.bitLength = bitLen;
    return seg;
}

struct qrcodegen_Segment qrcodegen_makeBytes(const uint8_t* dataAndLen, size_t len, uint8_t tempBuffer[]) {
    struct qrcodegen_Segment seg;
    seg.mode = qrcodegen_Mode_BYTE;
    seg.numChars = len;
    seg.data = (uint8_t*)dataAndLen;
    seg.bitLength = len * 8;
    (void)tempBuffer;
    return seg;
}

struct qrcodegen_Segment qrcodegen_makeKanji(const uint8_t* dataAndLen, size_t len, uint8_t tempBuffer[]) {
    struct qrcodegen_Segment seg;
    seg.mode = qrcodegen_Mode_KANJI;
    seg.numChars = len;
    seg.data = (uint8_t*)dataAndLen;
    seg.bitLength = len * 13;
    (void)tempBuffer;
    return seg;
}

bool qrcodegen_isAlphanumeric(const char* text) {
    while (*text) {
        if (ALPHANUMERIC_TABLE[(uint8_t)*text] < 0) return FALSE;
        text++;
    }
    return TRUE;
}

bool qrcodegen_isNumeric(const char* text) {
    while (*text) {
        if (NUMERIC_TABLE[(uint8_t)*text] < 0) return FALSE;
        text++;
    }
    return TRUE;
}

bool qrcodegen_isKanji(const uint8_t* dataAndLen, size_t len) {
    (void)dataAndLen;
    (void)len;
    return FALSE;
}

#pragma GCC diagnostic pop
