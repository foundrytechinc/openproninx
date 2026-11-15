// KBD
#include "kbdreg.h"
#include "defs.h"
#include "malloc.h"
#include "proc.h"

static const struct fnu_keymap key_map = { 0x6d, {
{{   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP, }, 0xFF,0x00 },
{{          0x1B,          0x1B,          0x1B,          0x1B,          0x1B,          0x1B,   FNU_KEY_DBG,          0x1B, }, 0x02,0x00 },
{{           '1',           '!',   FNU_KEY_NOP,   FNU_KEY_NOP,           '1',           '!',   FNU_KEY_NOP,   FNU_KEY_NOP, }, 0x33,0x00 },
{{           '2',           '@',          0x00,          0x00,           '2',           '@',          0x00,          0x00, }, 0x00,0x00 },
{{           '3',           '#',   FNU_KEY_NOP,   FNU_KEY_NOP,           '3',           '#',   FNU_KEY_NOP,   FNU_KEY_NOP, }, 0x33,0x00 },
{{           '4',           '$',   FNU_KEY_NOP,   FNU_KEY_NOP,           '4',           '$',   FNU_KEY_NOP,   FNU_KEY_NOP, }, 0x33,0x00 },
{{           '5',           '%',   FNU_KEY_NOP,   FNU_KEY_NOP,           '5',           '%',   FNU_KEY_NOP,   FNU_KEY_NOP, }, 0x33,0x00 },
{{           '6',           '^',          0x1E,          0x1E,           '6',           '^',          0x1E,          0x1E, }, 0x00,0x00 },
{{           '7',           '&',   FNU_KEY_NOP,   FNU_KEY_NOP,           '7',           '&',   FNU_KEY_NOP,   FNU_KEY_NOP, }, 0x33,0x00 },
{{           '8',           '*',   FNU_KEY_NOP,   FNU_KEY_NOP,           '8',           '*',   FNU_KEY_NOP,   FNU_KEY_NOP, }, 0x33,0x00 },
{{           '9',           '(',   FNU_KEY_NOP,   FNU_KEY_NOP,           '9',           '(',   FNU_KEY_NOP,   FNU_KEY_NOP, }, 0x33,0x00 },
{{           '0',           ')',   FNU_KEY_NOP,   FNU_KEY_NOP,           '0',           ')',   FNU_KEY_NOP,   FNU_KEY_NOP, }, 0x33,0x00 },
{{           '-',           '_',          0x1F,          0x1F,           '-',           '_',          0x1F,          0x1F, }, 0x00,0x00 },
{{           '=',           '+',   FNU_KEY_NOP,   FNU_KEY_NOP,           '=',           '+',   FNU_KEY_NOP,   FNU_KEY_NOP, }, 0x33,0x00 },
{{          0x08,          0x08,          0x7F,          0x7F,          0x08,          0x08,          0x7F,          0x7F, }, 0x00,0x00 },
{{          0x09,  FNU_KEY_BTAB,   FNU_KEY_NOP,   FNU_KEY_NOP,          0x09,  FNU_KEY_BTAB,   FNU_KEY_NOP,   FNU_KEY_NOP, }, 0x77,0x00 },
{{           'q',           'Q',          0x11,          0x11,           'q',           'Q',          0x11,          0x11, }, 0x00,0x01 },
{{           'w',           'W',          0x17,          0x17,           'w',           'W',          0x17,          0x17, }, 0x00,0x01 },
{{           'e',           'E',          0x05,          0x05,           'e',           'E',          0x05,          0x05, }, 0x00,0x01 },
{{           'r',           'R',          0x12,          0x12,           'r',           'R',          0x12,          0x12, }, 0x00,0x01 },
{{           't',           'T',          0x14,          0x14,           't',           'T',          0x14,          0x14, }, 0x00,0x01 },
{{           'y',           'Y',          0x19,          0x19,           'y',           'Y',          0x19,          0x19, }, 0x00,0x01 },
{{           'u',           'U',          0x15,          0x15,           'u',           'U',          0x15,          0x15, }, 0x00,0x01 },
{{           'i',           'I',          0x09,          0x09,           'i',           'I',          0x09,          0x09, }, 0x00,0x01 },
{{           'o',           'O',          0x0F,          0x0F,           'o',           'O',          0x0F,          0x0F, }, 0x00,0x01 },
{{           'p',           'P',          0x10,          0x10,           'p',           'P',          0x10,          0x10, }, 0x00,0x01 },
{{           '[',           '{',          0x1B,          0x1B,           '[',           '{',          0x1B,          0x1B, }, 0x00,0x00 },
{{           ']',           '}',          0x1D,          0x1D,           ']',           '}',          0x1D,          0x1D, }, 0x00,0x00 },
{{          0x0D,          0x0D,          0x0A,          0x0A,          0x0D,          0x0D,          0x0A,          0x0A, }, 0x00,0x00 },
{{  FNU_KEY_LCTR,  FNU_KEY_LCTR,  FNU_KEY_LCTR,  FNU_KEY_LCTR,  FNU_KEY_LCTR,  FNU_KEY_LCTR,  FNU_KEY_LCTR,  FNU_KEY_LCTR, }, 0xFF,0x00 },
{{           'a',           'A',          0x01,          0x01,           'a',           'A',          0x01,          0x01, }, 0x00,0x01 },
{{           's',           'S',          0x13,          0x13,           's',           'S',          0x13,          0x13, }, 0x00,0x01 },
{{           'd',           'D',          0x04,          0x04,           'd',           'D',          0x04,          0x04, }, 0x00,0x01 },
{{           'f',           'F',          0x06,          0x06,           'f',           'F',          0x06,          0x06, }, 0x00,0x01 },
{{           'g',           'G',          0x07,          0x07,           'g',           'G',          0x07,          0x07, }, 0x00,0x01 },
{{           'h',           'H',          0x08,          0x08,           'h',           'H',          0x08,          0x08, }, 0x00,0x01 },
{{           'j',           'J',          0x0A,          0x0A,           'j',           'J',          0x0A,          0x0A, }, 0x00,0x01 },
{{           'k',           'K',          0x0B,          0x0B,           'k',           'K',          0x0B,          0x0B, }, 0x00,0x01 },
{{           'l',           'L',          0x0C,          0x0C,           'l',           'L',          0x0C,          0x0C, }, 0x00,0x01 },
{{           ';',           ':',   FNU_KEY_NOP,   FNU_KEY_NOP,           ';',           ':',   FNU_KEY_NOP,   FNU_KEY_NOP, }, 0x33,0x00 },
{{          '\'',           '"',   FNU_KEY_NOP,   FNU_KEY_NOP,          '\'',           '"',   FNU_KEY_NOP,   FNU_KEY_NOP, }, 0x33,0x00 },
{{           '`',           '~',   FNU_KEY_NOP,   FNU_KEY_NOP,           '`',           '~',   FNU_KEY_NOP,   FNU_KEY_NOP, }, 0x33,0x00 },
{{   FNU_KEY_LSH,   FNU_KEY_LSH,   FNU_KEY_LSH,   FNU_KEY_LSH,   FNU_KEY_LSH,   FNU_KEY_LSH,   FNU_KEY_LSH,   FNU_KEY_LSH, }, 0xFF,0x00 },
{{          '\\',           '|',          0x1C,          0x1C,          '\\',           '|',          0x1C,          0x1C, }, 0x00,0x00 },
{{           'z',           'Z',          0x1A,          0x1A,           'z',           'Z',          0x1A,          0x1A, }, 0x00,0x01 },
{{           'x',           'X',          0x18,          0x18,           'x',           'X',          0x18,          0x18, }, 0x00,0x01 },
{{           'c',           'C',          0x03,          0x03,           'c',           'C',          0x03,          0x03, }, 0x00,0x01 },
{{           'v',           'V',          0x16,          0x16,           'v',           'V',          0x16,          0x16, }, 0x00,0x01 },
{{           'b',           'B',          0x02,          0x02,           'b',           'B',          0x02,          0x02, }, 0x00,0x01 },
{{           'n',           'N',          0x0E,          0x0E,           'n',           'N',          0x0E,          0x0E, }, 0x00,0x01 },
{{           'm',           'M',          0x0D,          0x0D,           'm',           'M',          0x0D,          0x0D, }, 0x00,0x01 },
{{           ',',           '<',   FNU_KEY_NOP,   FNU_KEY_NOP,           ',',           '<',   FNU_KEY_NOP,   FNU_KEY_NOP, }, 0x33,0x00 },
{{           '.',           '>',   FNU_KEY_NOP,   FNU_KEY_NOP,           '.',           '>',   FNU_KEY_NOP,   FNU_KEY_NOP, }, 0x33,0x00 },
{{           '/',           '?',   FNU_KEY_NOP,   FNU_KEY_NOP,           '/',           '?',   FNU_KEY_NOP,   FNU_KEY_NOP, }, 0x33,0x00 },
{{   FNU_KEY_RSH,   FNU_KEY_RSH,   FNU_KEY_RSH,   FNU_KEY_RSH,   FNU_KEY_RSH,   FNU_KEY_RSH,   FNU_KEY_RSH,   FNU_KEY_RSH, }, 0xFF,0x00 },
{{           '*',           '*',           '*',           '*',           '*',           '*',           '*',           '*', }, 0x00,0x00 },
{{  FNU_KEY_LALT,  FNU_KEY_LALT,  FNU_KEY_LALT,  FNU_KEY_LALT,  FNU_KEY_LALT,  FNU_KEY_LALT,  FNU_KEY_LALT,  FNU_KEY_LALT, }, 0xFF,0x00 },
{{           ' ',           ' ',          0x00,           ' ',           ' ',           ' ',  FNU_KEY_SUSP,           ' ', }, 0x02,0x00 },
{{   FNU_KEY_CLK,   FNU_KEY_CLK,   FNU_KEY_CLK,   FNU_KEY_CLK,   FNU_KEY_CLK,   FNU_KEY_CLK,   FNU_KEY_CLK,   FNU_KEY_CLK, }, 0xFF,0x00 },
{{ FNU_KEY_F( 1), FNU_KEY_F(13), FNU_KEY_F(25), FNU_KEY_F(37), FNU_KEY_S( 1), FNU_KEY_S(11), FNU_KEY_S( 1), FNU_KEY_S(11), }, 0xFF,0x00 },
{{ FNU_KEY_F( 2), FNU_KEY_F(14), FNU_KEY_F(26), FNU_KEY_F(38), FNU_KEY_S( 2), FNU_KEY_S(12), FNU_KEY_S( 2), FNU_KEY_S(12), }, 0xFF,0x00 },
{{ FNU_KEY_F( 3), FNU_KEY_F(15), FNU_KEY_F(27), FNU_KEY_F(39), FNU_KEY_S( 3), FNU_KEY_S(13), FNU_KEY_S( 3), FNU_KEY_S(13), }, 0xFF,0x00 },
{{ FNU_KEY_F( 4), FNU_KEY_F(16), FNU_KEY_F(28), FNU_KEY_F(40), FNU_KEY_S( 4), FNU_KEY_S(14), FNU_KEY_S( 4), FNU_KEY_S(14), }, 0xFF,0x00 },
{{ FNU_KEY_F( 5), FNU_KEY_F(17), FNU_KEY_F(29), FNU_KEY_F(41), FNU_KEY_S( 5), FNU_KEY_S(15), FNU_KEY_S( 5), FNU_KEY_S(15), }, 0xFF,0x00 },
{{ FNU_KEY_F( 6), FNU_KEY_F(18), FNU_KEY_F(30), FNU_KEY_F(42), FNU_KEY_S( 6), FNU_KEY_S(16), FNU_KEY_S( 6), FNU_KEY_S(16), }, 0xFF,0x00 },
{{ FNU_KEY_F( 7), FNU_KEY_F(19), FNU_KEY_F(31), FNU_KEY_F(43), FNU_KEY_S( 7), FNU_KEY_S( 7), FNU_KEY_S( 7), FNU_KEY_S( 7), }, 0xFF,0x00 },
{{ FNU_KEY_F( 8), FNU_KEY_F(20), FNU_KEY_F(32), FNU_KEY_F(44), FNU_KEY_S( 8), FNU_KEY_S( 8), FNU_KEY_S( 8), FNU_KEY_S( 8), }, 0xFF,0x00 },
{{ FNU_KEY_F( 9), FNU_KEY_F(21), FNU_KEY_F(33), FNU_KEY_F(45), FNU_KEY_S( 9), FNU_KEY_S( 9), FNU_KEY_S( 9), FNU_KEY_S( 9), }, 0xFF,0x00 },
{{ FNU_KEY_F(10), FNU_KEY_F(22), FNU_KEY_F(34), FNU_KEY_F(46), FNU_KEY_S(10), FNU_KEY_S(10), FNU_KEY_S(10), FNU_KEY_S(10), }, 0xFF,0x00 },
{{   FNU_KEY_NLK,   FNU_KEY_NLK,   FNU_KEY_NLK,   FNU_KEY_NLK,   FNU_KEY_NLK,   FNU_KEY_NLK,   FNU_KEY_NLK,   FNU_KEY_NLK, }, 0xFF,0x00 },
{{   FNU_KEY_SLK,   FNU_KEY_SLK,   FNU_KEY_SLK,   FNU_KEY_SLK,   FNU_KEY_SLK,   FNU_KEY_SLK,   FNU_KEY_SLK,   FNU_KEY_SLK, }, 0xFF,0x00 },
{{ FNU_KEY_F(49),           '7',           '7',           '7',           '7',           '7',           '7',           '7', }, 0x80,0x02 },
{{ FNU_KEY_F(50),           '8',           '8',           '8',           '8',           '8',           '8',           '8', }, 0x80,0x02 },
{{ FNU_KEY_F(51),           '9',           '9',           '9',           '9',           '9',           '9',           '9', }, 0x80,0x02 },
{{ FNU_KEY_F(52),           '-',           '-',           '-',           '-',           '-',           '-',           '-', }, 0x80,0x02 },
{{ FNU_KEY_F(53),           '4',           '4',           '4',           '4',           '4',           '4',           '4', }, 0x80,0x02 },
{{ FNU_KEY_F(54),           '5',           '5',           '5',           '5',           '5',           '5',           '5', }, 0x80,0x02 },
{{ FNU_KEY_F(55),           '6',           '6',           '6',           '6',           '6',           '6',           '6', }, 0x80,0x02 },
{{ FNU_KEY_F(56),           '+',           '+',           '+',           '+',           '+',           '+',           '+', }, 0x80,0x02 },
{{ FNU_KEY_F(57),           '1',           '1',           '1',           '1',           '1',           '1',           '1', }, 0x80,0x02 },
{{ FNU_KEY_F(58),           '2',           '2',           '2',           '2',           '2',           '2',           '2', }, 0x80,0x02 },
{{ FNU_KEY_F(59),           '3',           '3',           '3',           '3',           '3',           '3',           '3', }, 0x80,0x02 },
{{ FNU_KEY_F(60),           '0',           '0',           '0',           '0',           '0',           '0',           '0', }, 0x80,0x02 },
{{          0x7F,           '.',           '.',           '.',           '.',           '.',   FNU_KEY_RBT,   FNU_KEY_RBT, }, 0x03,0x02 },
{{   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP, }, 0xFF,0x00 },
{{   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP, }, 0xFF,0x00 },
{{   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP, }, 0xFF,0x00 },
{{ FNU_KEY_F(11), FNU_KEY_F(23), FNU_KEY_F(35), FNU_KEY_F(47), FNU_KEY_S(11), FNU_KEY_S(11), FNU_KEY_S(11), FNU_KEY_S(11), }, 0xFF,0x00 },
{{ FNU_KEY_F(12), FNU_KEY_F(24), FNU_KEY_F(36), FNU_KEY_F(48), FNU_KEY_S(12), FNU_KEY_S(12), FNU_KEY_S(12), FNU_KEY_S(12), }, 0xFF,0x00 },
{{          0x0D,          0x0D,          0x0A,          0x0A,          0x0D,          0x0D,          0x0A,          0x0A, }, 0x00,0x00 },
{{  FNU_KEY_RCTR,  FNU_KEY_RCTR,  FNU_KEY_RCTR,  FNU_KEY_RCTR,  FNU_KEY_RCTR,  FNU_KEY_RCTR,  FNU_KEY_RCTR,  FNU_KEY_RCTR, }, 0xFF,0x00 },
{{           '/',           '/',           '/',           '/',           '/',           '/',           '/',           '/', }, 0x00,0x02 },
{{  FNU_KEY_NEXT,  FNU_KEY_PREV,   FNU_KEY_DBG,   FNU_KEY_DBG,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP, }, 0xFF,0x00 },
{{  FNU_KEY_RALT,  FNU_KEY_RALT,  FNU_KEY_RALT,  FNU_KEY_RALT,  FNU_KEY_RALT,  FNU_KEY_RALT,  FNU_KEY_RALT,  FNU_KEY_RALT, }, 0xFF,0x00 },
{{ FNU_KEY_F(49), FNU_KEY_F(49), FNU_KEY_F(49), FNU_KEY_F(49), FNU_KEY_F(49), FNU_KEY_F(49), FNU_KEY_F(49), FNU_KEY_F(49), }, 0xFF,0x00 },
{{ FNU_KEY_F(50), FNU_KEY_F(50), FNU_KEY_F(50), FNU_KEY_F(50), FNU_KEY_F(50), FNU_KEY_F(50), FNU_KEY_F(50), FNU_KEY_F(50), }, 0xFF,0x00 },
{{ FNU_KEY_F(51), FNU_KEY_F(51), FNU_KEY_F(51), FNU_KEY_F(51), FNU_KEY_F(51), FNU_KEY_F(51), FNU_KEY_F(51), FNU_KEY_F(51), }, 0xFF,0x00 },
{{ FNU_KEY_F(53), FNU_KEY_F(53), FNU_KEY_F(53), FNU_KEY_F(53), FNU_KEY_F(53), FNU_KEY_F(53), FNU_KEY_F(53), FNU_KEY_F(53), }, 0xFF,0x00 },
{{ FNU_KEY_F(55), FNU_KEY_F(55), FNU_KEY_F(55), FNU_KEY_F(55), FNU_KEY_F(55), FNU_KEY_F(55), FNU_KEY_F(55), FNU_KEY_F(55), }, 0xFF,0x00 },
{{ FNU_KEY_F(57), FNU_KEY_F(57), FNU_KEY_F(57), FNU_KEY_F(57), FNU_KEY_F(57), FNU_KEY_F(57), FNU_KEY_F(57), FNU_KEY_F(57), }, 0xFF,0x00 },
{{ FNU_KEY_F(58), FNU_KEY_F(58), FNU_KEY_F(58), FNU_KEY_F(58), FNU_KEY_F(58), FNU_KEY_F(58), FNU_KEY_F(58), FNU_KEY_F(58), }, 0xFF,0x00 },
{{ FNU_KEY_F(59), FNU_KEY_F(59), FNU_KEY_F(59), FNU_KEY_F(59), FNU_KEY_F(59), FNU_KEY_F(59), FNU_KEY_F(59), FNU_KEY_F(59), }, 0xFF,0x00 },
{{ FNU_KEY_F(60), FNU_KEY_PASTE, FNU_KEY_F(60), FNU_KEY_F(60), FNU_KEY_F(60), FNU_KEY_F(60), FNU_KEY_F(60), FNU_KEY_F(60), }, 0xFF,0x00 },
{{ FNU_KEY_F(61), FNU_KEY_F(61), FNU_KEY_F(61), FNU_KEY_F(61), FNU_KEY_F(61), FNU_KEY_F(61),   FNU_KEY_RBT, FNU_KEY_F(61), }, 0xFF,0x00 },
{{   FNU_KEY_SLK,  FNU_KEY_SPSC,   FNU_KEY_SLK,  FNU_KEY_SPSC,  FNU_KEY_SUSP,   FNU_KEY_NOP,  FNU_KEY_SUSP,   FNU_KEY_NOP, }, 0xFF,0x00 },
{{ FNU_KEY_F(62), FNU_KEY_F(62), FNU_KEY_F(62), FNU_KEY_F(62), FNU_KEY_F(62), FNU_KEY_F(62), FNU_KEY_F(62), FNU_KEY_F(62), }, 0xFF,0x00 },
{{ FNU_KEY_F(63), FNU_KEY_F(63), FNU_KEY_F(63), FNU_KEY_F(63), FNU_KEY_F(63), FNU_KEY_F(63), FNU_KEY_F(63), FNU_KEY_F(63), }, 0xFF,0x00 },
{{ FNU_KEY_F(64), FNU_KEY_F(64), FNU_KEY_F(64), FNU_KEY_F(64), FNU_KEY_F(64), FNU_KEY_F(64), FNU_KEY_F(64), FNU_KEY_F(64), }, 0xFF,0x00 },
{{   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP,   FNU_KEY_NOP, }, 0xFF,0x00 },
} };

static const struct fnu_accentmap accent_map = { 15,
  {
    {  '`', { {  'a',0xe0 }, {  'A',0xc0 }, {  'e',0xe8 }, {  'E',0xc8 },
	      {  'i',0xec }, {  'I',0xcc }, {  'o',0xf2 }, {  'O',0xd2 },
	      {  'u',0xf9 }, {  'U',0xd9 }, }, },
    { 0xb4, { {  'a',0xe1 }, {  'A',0xc1 }, {  'e',0xe9 }, {  'E',0xc9 },
	      {  'i',0xed }, {  'I',0xcd }, {  'o',0xf3 }, {  'O',0xd3 },
	      {  'u',0xfa }, {  'U',0xda }, {  'y',0xfd }, {  'Y',0xdd }, }, },
    {  '^', { {  'a',0xe2 }, {  'A',0xc2 }, {  'e',0xea }, {  'E',0xca },
	      {  'i',0xee }, {  'I',0xce }, {  'o',0xf4 }, {  'O',0xd4 },
	      {  'u',0xfb }, {  'U',0xdb }, }, },
    {  '~', { {  'a',0xe3 }, {  'A',0xc3 }, {  'n',0xf1 }, {  'N',0xd1 },
	      {  'o',0xf5 }, {  'O',0xd5 }, }, },
    { 0 },
    { 0 },
    { 0 },
    { 0xa8, { {  'a',0xe4 }, {  'A',0xc4 }, {  'e',0xeb }, {  'E',0xcb },
	      {  'i',0xef }, {  'I',0xcf }, {  'o',0xf6 }, {  'O',0xd6 },
	      {  'u',0xfc }, {  'U',0xdc }, {  'y',0xff }, }, },
    { 0 },
    { 0xb0, { {  'a',0xe5 }, {  'A',0xc5 }, }, },
    { 0xb8, { {  'c',0xe7 }, {  'C',0xc7 }, }, },
    { 0 },
    { 0 },
    { 0 },
    { 0 },
  }
};

static const struct fnu_fkeytab fkey_tab[96] = {
	{"\033[M", 3}, {"\033[N", 3}, {"\033[O", 3}, {"\033[P", 3},
	{"\033[Q", 3}, {"\033[R", 3}, {"\033[S", 3}, {"\033[T", 3},
	{"\033[U", 3}, {"\033[V", 3}, {"\033[W", 3}, {"\033[X", 3},
	{"\033[Y", 3}, {"\033[Z", 3}, {"\033[a", 3}, {"\033[b", 3},
	{"\033[c", 3}, {"\033[d", 3}, {"\033[e", 3}, {"\033[f", 3},
	{"\033[g", 3}, {"\033[h", 3}, {"\033[i", 3}, {"\033[j", 3},
	{"\033[k", 3}, {"\033[l", 3}, {"\033[m", 3}, {"\033[n", 3},
	{"\033[o", 3}, {"\033[p", 3}, {"\033[q", 3}, {"\033[r", 3},
	{"\033[s", 3}, {"\033[t", 3}, {"\033[u", 3}, {"\033[v", 3},
	{"\033[w", 3}, {"\033[x", 3}, {"\033[y", 3}, {"\033[z", 3},
	{"\033[@", 3}, {"\033[[", 3}, {"\033[\\",3}, {"\033[]", 3},
     {"\033[^", 3}, {"\033[_", 3}, {"\033[`", 3}, {"\033[{", 3},
	{"\033[H", 3}, {"\033[A", 3}, {"\033[I", 3}, {"-"     , 1},
	{"\033[D", 3}, {"\033[E", 3}, {"\033[C", 3}, {"+"     , 1},
	{"\033[F", 3}, {"\033[B", 3}, {"\033[G", 3}, {"\033[L", 3},
	{"\177", 1},   {"\033[J", 3}, {"\033[~", 3}, {"\033[}", 3},
	{"", 0}      , {"", 0}      , {"", 0}      , {"", 0}      ,
	{"", 0}      , {"", 0}      , {"", 0}      , {"", 0}      ,
	{"", 0}      , {"", 0}      , {"", 0}      , {"", 0}      ,
	{"", 0}      , {"", 0}      , {"", 0}      , {"", 0}      ,
	{"", 0}      , {"", 0}      , {"", 0}      , {"", 0}      ,
	{"", 0}      , {"", 0}      , {"", 0}      , {"", 0}      ,
	{"", 0}      , {"", 0}      , {"", 0}      , {"", 0}      ,
	{"", 0}      , {"", 0}      , {"", 0}      , {"", 0}
};

struct mutex kbd_lock = MUTEX_INITIALIZER("kbd");
static keyboard_t *keyboards; // by kb_index

static inline int imin(int a, int b) { return a < b ? a : b; }

void kbd_init_struct(keyboard_t *kbd, const char *name, int type, int unit,
                     int config) {
  kbd->kb_flags = KB_NO_DEVICE;
  kbd->kb_name = name;
  kbd->kb_type = type;
  kbd->kb_unit = unit;
  kbd->kb_config = config & ~KB_CONF_PROBE_ONLY;
  kbd->kb_led = 0;
  kbd->kb_data = 0;
  kbd->kb_keymap = 0;
  kbd->kb_accentmap = 0;
  kbd->kb_fkeytab = 0;
  kbd->kb_fkeytab_size = 0;
  kbd->kb_delay1 = KBD_DELAY1;
  kbd->kb_delay2 = KBD_DELAY2;
  memset(kbd->kb_lastact, 0, sizeof(kbd->kb_lastact));
}

void kbd_set_maps(keyboard_t *kbd, struct fnu_keymap *keymap,
                  struct fnu_accentmap *accmap, struct fnu_fkeytab *fkeymap,
                  int fkeymap_size) {
  kbd->kb_keymap = keymap;
  kbd->kb_accentmap = accmap;
  kbd->kb_fkeytab = fkeymap;
  kbd->kb_fkeytab_size = fkeymap_size;
}

void kbd_default_maps(struct fnu_keymap *keymap, struct fnu_accentmap *accmap,
                      struct fnu_fkeytab *fkeymap) {
  memmove(keymap, &key_map, sizeof(key_map));
  memmove(accmap, &accent_map, sizeof(accent_map));
  memmove(fkeymap, fkey_tab, sizeof(fkey_tab));
}

// the lowest index free
int kbd_register(keyboard_t *kbd) {
  keyboard_t **pp;
  int index = 0;

  mtx_enter(&kbd_lock);
  for (pp = &keyboards; *pp != 0 && (*pp)->kb_index == index; pp = &(*pp)->kb_next)
    index++;
  kbd->kb_index = index;
  kbd->kb_next = *pp;
  *pp = kbd;
  KBD_VALID(kbd);
  mtx_leave(&kbd_lock);
  return index;
}

void kbd_unregister(keyboard_t *kbd) {
  keyboard_t **pp;

  mtx_enter(&kbd_lock);
  for (pp = &keyboards; *pp != 0; pp = &(*pp)->kb_next)
    if (*pp == kbd) {
      *pp = kbd->kb_next;
      break;
    }
  KBD_INVALID(kbd);
  mtx_leave(&kbd_lock);
}

const char *kbd_type_name(int type) {
  switch (type) {
  case FNU_KB_84: return "AT 84";
  case FNU_KB_101: return "AT 101/102";
  default: return "unknown";
  }
}

int kbd_scancode_keycode(keyboard_t *kbd __unused, uchar *prefix, int scancode) {
  int keycode = scancode & 0x7f;

  switch (*prefix) {
  case 0x00:
    switch (scancode) {
    case 0xe0:
    case 0xe1:
      *prefix = (uchar)scancode;
      return -1;
    }
    break;
  case 0xe0:
    *prefix = 0;
    switch (keycode) {
    case 0x1c: keycode = 0x59; break;
    case 0x1d: keycode = 0x5a; break;
    case 0x35: keycode = 0x5b; break;
    case 0x37: keycode = 0x5c; break;
    case 0x38: keycode = 0x5d; break;
    case 0x46: keycode = 0x68; break;
    case 0x47: keycode = 0x5e; break;
    case 0x48: keycode = 0x5f; break;
    case 0x49: keycode = 0x60; break;
    case 0x4b: keycode = 0x61; break;
    case 0x4d: keycode = 0x62; break;
    case 0x4f: keycode = 0x63; break;
    case 0x50: keycode = 0x64; break;
    case 0x51: keycode = 0x65; break;
    case 0x52: keycode = 0x66; break;
    case 0x53: keycode = 0x67; break;
    case 0x5b: keycode = 0x69; break;
    case 0x5c: keycode = 0x6a; break;
    case 0x5d: keycode = 0x6b; break;
    case 0x5e: keycode = 0x6d; break;
    case 0x5f: keycode = 0x6e; break;
    case 0x63: keycode = 0x6f; break;
    default: return -1;
    }
    break;
  case 0xe1:
    *prefix = 0;
    if (keycode == 0x1d)
      *prefix = 0x1d;
    return -1;
  case 0x1d:
    *prefix = 0;
    if (keycode != 0x45)
      return -1;
    keycode = 0x68;
    break;
  }
  return keycode;
}

#define set_lockkey_state(k, s, l)                                             \
  if (!((s) & l##DOWN)) {                                                      \
    int i_;                                                                    \
    (s) |= l##DOWN;                                                            \
    (s) ^= FNU_KB_##l##ED;                                                     \
    i_ = (s) & FNU_KB_LOCKS;                                                   \
    (void)kbdd_ioctl((k), FNU_KBSETLEDS, &i_);                                 \
  }

static uint32 save_accent_key(keyboard_t *kbd, uint32 key, int *accents) {
  int i;

  i = key - FNU_KEY_F_ACC + 1;
  if ((i > kbd->kb_accentmap->n_accs) || (kbd->kb_accentmap->acc[i - 1].accchar == 0)) {
    *accents = 0;
    return FNU_ERRKEY;
  }

  if (i == *accents) {
    key = kbd->kb_accentmap->acc[i - 1].accchar;
    *accents = 0;
    return key;
  }

  *accents = i;
  return FNU_NOKEY;
}

static uint32 make_accent_char(keyboard_t *kbd, uint32 ch, int *accents) {
  struct fnu_accent *acc;
  int i;

  acc = &kbd->kb_accentmap->acc[*accents - 1];
  *accents = 0;

  if (ch == ' ')
    return acc->accchar;

  for (i = 0; i < FNU_KB_NACCENTCHARS; ++i) {
    if (acc->map[i][0] == 0)
      break;
    if (acc->map[i][0] == ch)
      return acc->map[i][1];
  }
  return FNU_ERRKEY;
}

int genkbd_keyaction(keyboard_t *kbd, int keycode, int up, int *shiftstate,
                     int *accents) {
  struct fnu_keyent *key;
  int state = *shiftstate;
  int action;
  int f;
  int i;

  i = keycode;
  f = state & (AGRS | FNU_KB_ALKED);
  if ((f == AGRS1) || (f == AGRS2) || (f == FNU_KB_ALKED))
    i += FNU_KB_ALTGR;
  key = &kbd->kb_keymap->key[i];
  i = ((state & SHIFTS) ? 1 : 0) | ((state & CTLS) ? 2 : 0) | ((state & ALTS) ? 4 : 0);
  if (((key->flgs & FNU_KEY_LOCK_C) && (state & FNU_KB_CLKED)) ||
      ((key->flgs & FNU_KEY_LOCK_N) && (state & FNU_KB_NLKED)))
    i ^= 1;

  if (up) {
    action = kbd->kb_lastact[keycode];
    kbd->kb_lastact[keycode] = FNU_KEY_NOP;
    switch (action) {
    case FNU_KEY_LSHA:
      if (state & SHIFTAON) {
        set_lockkey_state(kbd, state, ALK);
        state &= ~ALKDOWN;
      }
      action = FNU_KEY_LSH;
      // fallthrough
    case FNU_KEY_LSH:
      state &= ~SHIFTS1;
      break;
    case FNU_KEY_RSHA:
      if (state & SHIFTAON) {
        set_lockkey_state(kbd, state, ALK);
        state &= ~ALKDOWN;
      }
      action = FNU_KEY_RSH;
      // fallthrough
    case FNU_KEY_RSH:
      state &= ~SHIFTS2;
      break;
    case FNU_KEY_LCTRA:
      if (state & SHIFTAON) {
        set_lockkey_state(kbd, state, ALK);
        state &= ~ALKDOWN;
      }
      action = FNU_KEY_LCTR;
      // fallthrough
    case FNU_KEY_LCTR:
      state &= ~CTLS1;
      break;
    case FNU_KEY_RCTRA:
      if (state & SHIFTAON) {
        set_lockkey_state(kbd, state, ALK);
        state &= ~ALKDOWN;
      }
      action = FNU_KEY_RCTR;
      // fallthrough
    case FNU_KEY_RCTR:
      state &= ~CTLS2;
      break;
    case FNU_KEY_LALTA:
      if (state & SHIFTAON) {
        set_lockkey_state(kbd, state, ALK);
        state &= ~ALKDOWN;
      }
      action = FNU_KEY_LALT;
      // fallthrough
    case FNU_KEY_LALT:
      state &= ~ALTS1;
      break;
    case FNU_KEY_RALTA:
      if (state & SHIFTAON) {
        set_lockkey_state(kbd, state, ALK);
        state &= ~ALKDOWN;
      }
      action = FNU_KEY_RALT;
      // fallthrough
    case FNU_KEY_RALT:
      state &= ~ALTS2;
      break;
    case FNU_KEY_ASH:
      state &= ~AGRS1;
      break;
    case FNU_KEY_META:
      state &= ~METAS1;
      break;
    case FNU_KEY_NLK:
      state &= ~NLKDOWN;
      break;
    case FNU_KEY_CLK:
      state &= ~CLKDOWN;
      break;
    case FNU_KEY_SLK:
      state &= ~SLKDOWN;
      break;
    case FNU_KEY_ALK:
      state &= ~ALKDOWN;
      break;
    case FNU_KEY_NOP:
      *shiftstate &= ~SHIFTAON;
      return FNU_NOKEY;
    }
    *shiftstate = state & ~SHIFTAON;
    return FNU_SPCLKEY | FNU_RELKEY | action;
  } else {
    action = key->map[i];
    state &= ~SHIFTAON;
    if (key->spcl & (0x80 >> i)) {
      if (kbd->kb_lastact[keycode] == FNU_KEY_NOP)
        kbd->kb_lastact[keycode] = (uchar)action;
      if (kbd->kb_lastact[keycode] != action)
        action = FNU_KEY_NOP;
      switch (action) {
      case FNU_KEY_NLK:
        set_lockkey_state(kbd, state, NLK);
        break;
      case FNU_KEY_CLK:
        set_lockkey_state(kbd, state, CLK);
        break;
      case FNU_KEY_SLK:
        set_lockkey_state(kbd, state, SLK);
        break;
      case FNU_KEY_ALK:
        set_lockkey_state(kbd, state, ALK);
        break;
      case FNU_KEY_SPSC: case FNU_KEY_RBT: case FNU_KEY_SUSP: case FNU_KEY_STBY:
      case FNU_KEY_DBG: case FNU_KEY_NEXT: case FNU_KEY_PREV: case FNU_KEY_PNC:
      case FNU_KEY_HALT: case FNU_KEY_PDWN:
        *accents = 0;
        break;
      case FNU_KEY_BTAB:
        *accents = 0;
        action |= FNU_BKEY;
        break;
      case FNU_KEY_LSHA:
        state |= SHIFTAON;
        action = FNU_KEY_LSH;
        // fallthrough
      case FNU_KEY_LSH:
        state |= SHIFTS1;
        break;
      case FNU_KEY_RSHA:
        state |= SHIFTAON;
        action = FNU_KEY_RSH;
        // fallthrough
      case FNU_KEY_RSH:
        state |= SHIFTS2;
        break;
      case FNU_KEY_LCTRA:
        state |= SHIFTAON;
        action = FNU_KEY_LCTR;
        // fallthrough
      case FNU_KEY_LCTR:
        state |= CTLS1;
        break;
      case FNU_KEY_RCTRA:
        state |= SHIFTAON;
        action = FNU_KEY_RCTR;
        // fallthrough
      case FNU_KEY_RCTR:
        state |= CTLS2;
        break;
      case FNU_KEY_LALTA:
        state |= SHIFTAON;
        action = FNU_KEY_LALT;
        // fallthrough
      case FNU_KEY_LALT:
        state |= ALTS1;
        break;
      case FNU_KEY_RALTA:
        state |= SHIFTAON;
        action = FNU_KEY_RALT;
        // fallthrough
      case FNU_KEY_RALT:
        state |= ALTS2;
        break;
      case FNU_KEY_ASH:
        state |= AGRS1;
        break;
      case FNU_KEY_META:
        state |= METAS1;
        break;
      case FNU_KEY_NOP:
        *shiftstate = state;
        return FNU_NOKEY;
      default:
        *shiftstate = state;
        if (action >= FNU_KEY_F_ACC && action <= FNU_KEY_L_ACC) {
          action = (int)save_accent_key(kbd, (uint32)action, accents);
          switch (action) {
          case FNU_NOKEY:
          case FNU_ERRKEY:
            return action;
          default:
            if (state & METAS)
              return action | FNU_MKEY;
            else
              return action;
          }
        }
        if (*accents > 0) {
          *accents = 0;
          return FNU_ERRKEY;
        }
        if (action >= FNU_KEY_F_FN && action <= FNU_KEY_L_FN)
          action |= FNU_FKEY;
        return FNU_SPCLKEY | action;
      }
      *shiftstate = state;
      return FNU_SPCLKEY | action;
    } else {
      kbd->kb_lastact[keycode] = FNU_KEY_NOP;
      *shiftstate = state;
      if (*accents > 0) {
        action = (int)make_accent_char(kbd, (uint32)action, accents);
        if (action == FNU_ERRKEY)
          return action;
      }
      if (state & METAS)
        action |= FNU_MKEY;
      return action;
    }
  }
}

int genkbd_commonioctl(keyboard_t *kbd, uint64_t cmd, void *arg) {
  struct fnu_keyarg *keyp;
  struct fnu_fkeyarg *fkeyp;
  int i;

  switch (cmd) {
  case FNU_KBINFO:
    ((struct fnu_kbinfo *)arg)->kb_index = kbd->kb_index;
    i = imin((int)strlen(kbd->kb_name) + 1, (int)sizeof(((struct fnu_kbinfo *)arg)->kb_name));
    memmove(((struct fnu_kbinfo *)arg)->kb_name, kbd->kb_name, i);
    ((struct fnu_kbinfo *)arg)->kb_unit = kbd->kb_unit;
    ((struct fnu_kbinfo *)arg)->kb_type = kbd->kb_type;
    ((struct fnu_kbinfo *)arg)->kb_config = kbd->kb_config;
    ((struct fnu_kbinfo *)arg)->kb_flags = kbd->kb_flags;
    break;

  case FNU_KBGETTYPE:
    *(int *)arg = kbd->kb_type;
    break;

  case FNU_KBGETREPEAT:
    ((int *)arg)[0] = kbd->kb_delay1;
    ((int *)arg)[1] = kbd->kb_delay2;
    break;

  case FNU_KBGETMAP:
    memmove(arg, kbd->kb_keymap, sizeof(struct fnu_keymap));
    break;
  case FNU_KBSETMAP:
    memset(kbd->kb_accentmap, 0, sizeof(*kbd->kb_accentmap));
    memmove(kbd->kb_keymap, arg, sizeof(*kbd->kb_keymap));
    break;

  case FNU_KBGETKEY:
    keyp = (struct fnu_keyarg *)arg;
    if (keyp->keynum >= FNU_KB_NKEYS)
      return FNU_ERR_INVALID;
    memmove(&keyp->key, &kbd->kb_keymap->key[keyp->keynum], sizeof(keyp->key));
    break;
  case FNU_KBSETKEY:
    keyp = (struct fnu_keyarg *)arg;
    if (keyp->keynum >= FNU_KB_NKEYS)
      return FNU_ERR_INVALID;
    memmove(&kbd->kb_keymap->key[keyp->keynum], &keyp->key, sizeof(keyp->key));
    break;

  case FNU_KBGETACCENTS:
    memmove(arg, kbd->kb_accentmap, sizeof(struct fnu_accentmap));
    break;
  case FNU_KBSETACCENTS:
    memmove(kbd->kb_accentmap, arg, sizeof(*kbd->kb_accentmap));
    break;

  case FNU_KBGETFKEY:
    fkeyp = (struct fnu_fkeyarg *)arg;
    if (fkeyp->keynum >= kbd->kb_fkeytab_size)
      return FNU_ERR_INVALID;
    memmove(fkeyp->keydef, kbd->kb_fkeytab[fkeyp->keynum].str,
            kbd->kb_fkeytab[fkeyp->keynum].len);
    fkeyp->flen = (char)kbd->kb_fkeytab[fkeyp->keynum].len;
    break;
  case FNU_KBSETFKEY:
    fkeyp = (struct fnu_fkeyarg *)arg;
    if (fkeyp->keynum >= kbd->kb_fkeytab_size)
      return FNU_ERR_INVALID;
    kbd->kb_fkeytab[fkeyp->keynum].len = (uchar)imin(fkeyp->flen, FNU_KB_FKEYLEN);
    memmove(kbd->kb_fkeytab[fkeyp->keynum].str, fkeyp->keydef,
            kbd->kb_fkeytab[fkeyp->keynum].len);
    break;

  default:
    return FNU_ERR_BADREQUEST;
  }
  return 0;
}

// kbd_lock held
int kbd_lockstate(void) {
  int state = 0;

  if (keyboards != 0)
    kbdd_ioctl(keyboards, FNU_KBGETLOCKS, &state);
  return state;
}

void kbd_set_lockstate(int state) {
  keyboard_t *k;
  int s;

  for (k = keyboards; k != 0; k = k->kb_next) {
    s = state;
    kbdd_ioctl(k, FNU_KBSETLOCKS, &s);
  }
}

int kbd_ioctl_user(uint64_t cmd, uint64_t arg) {
  union {
    struct fnu_keymap keymap;
    struct fnu_accentmap accmap;
    struct fnu_keyarg keyarg;
    struct fnu_fkeyarg fkeyarg;
    struct fnu_kbinfo info;
    int ints[2];
  } *u;
  struct proc *p = curproc;
  keyboard_t *k;
  int size, n = 0, r, err = FNU_ERR_WRONGDEVICE, ask;

  size = (int)IOCPARM_LEN(cmd);
  switch (cmd) {
  case FNU_KBGETMODE: case FNU_KBGETLOCKS: case FNU_KBGETTYPE:
  case FNU_KBGETLEDS: case FNU_KBINFO: case FNU_KBGETREPEAT: case FNU_KBGETKEY:
  case FNU_KBGETFKEY:
    ask = 1;
    break;
  case FNU_KBSETMODE: case FNU_KBSETLOCKS: case FNU_KBSETLEDS:
  case FNU_KBSETRATE: case FNU_KBSETREPEAT: case FNU_KBSETKEY:
  case FNU_KBSETFKEY:
    ask = 0;
    break;
  case FNU_KBGETMAP: case FNU_KBSETMAP:
    size = sizeof(struct fnu_keymap);
    ask = cmd == FNU_KBGETMAP;
    break;
  case FNU_KBGETACCENTS: case FNU_KBSETACCENTS:
    size = sizeof(struct fnu_accentmap);
    ask = cmd == FNU_KBGETACCENTS;
    break;
  default:
    return FNU_ERR_BADREQUEST;
  }
  u = malloc(sizeof(*u), M_TEMP, M_WAITOK | M_ZERO);
  if ((cmd & IOC_DIRMASK) == IOC_VOID && IOCPARM_LEN(cmd) == sizeof(int))
    u->ints[0] = (int)arg;
  else if (((cmd & IOC_IN) || !ask) &&
           copyin(p->p_p->pgdir, u, (uintptr_t)arg, size) < 0) {
    free(u, M_TEMP);
    return FNU_ERR_FAULT;
  }
  mtx_enter(&kbd_lock);
  for (k = keyboards; k != 0; k = k->kb_next) {
    r = kbdd_ioctl(k, cmd, u);
    if (n++ == 0 || err == 0)
      err = r;
    if (ask)
      break;
  }
  mtx_leave(&kbd_lock);
  if (err == 0 && ask && copyout(p->p_p->pgdir, (uintptr_t)arg, u, size) < 0)
    err = FNU_ERR_FAULT;
  free(u, M_TEMP);
  return err;
}

// THE SCAN CODES

uint32 genkbd_pending(struct kbdxlate *x) {
  uint action;

  if (!(x->flags & COMPOSE) && x->composed_char > 0) {
    action = x->composed_char;
    x->composed_char = 0;
    if (action > 255)
      return FNU_ERRKEY;
    return action;
  }
  return FNU_NOKEY;
}

// FNU_NOKEY
uint32 genkbd_scancode(keyboard_t *kbd, struct kbdxlate *x, int scancode) {
  int keycode;
  uint action;

  if (x->mode == FNU_KB_RAW)
    return (uint32)scancode;

  if (x->prefix == 0) {
    switch (scancode) {
    case 0xb8:
      if (x->flags & COMPOSE) {
        x->flags &= ~COMPOSE;
        if (x->composed_char > 255)
          x->composed_char = 0;
      }
      break;
    case 0x38:
      if (!(x->flags & COMPOSE)) {
        x->flags |= COMPOSE;
        x->composed_char = 0;
      }
      break;
    }
  }
  keycode = kbd_scancode_keycode(kbd, &x->prefix, scancode);
  if (keycode < 0)
    return FNU_NOKEY;

  if (kbd->kb_type == FNU_KB_84) {
    switch (keycode) {
    case 0x37:
      if (x->state & SHIFTS)
        keycode = 0x5c;
      break;
    case 0x45:
      if (x->state & CTLS)
        keycode = 0x68;
      break;
    case 0x46:
      if (x->state & CTLS)
        keycode = 0x6c;
      break;
    }
  } else if (kbd->kb_type == FNU_KB_101) {
    switch (keycode) {
    case 0x5c:
      if (x->state & ALTS)
        keycode = 0x54;
      break;
    case 0x68:
      if (x->state & CTLS)
        keycode = 0x6c;
      break;
    }
  }

  if (x->mode == FNU_KB_CODE)
    return (uint32)(keycode | (scancode & 0x80));

  if (x->flags & COMPOSE) {
    switch (keycode | (scancode & 0x80)) {
    case 0x47: case 0x48: case 0x49:
      x->composed_char *= 10;
      x->composed_char += keycode - 0x40;
      if (x->composed_char > 255)
        return FNU_ERRKEY;
      return FNU_NOKEY;
    case 0x4b: case 0x4c: case 0x4d:
      x->composed_char *= 10;
      x->composed_char += keycode - 0x47;
      if (x->composed_char > 255)
        return FNU_ERRKEY;
      return FNU_NOKEY;
    case 0x4f: case 0x50: case 0x51:
      x->composed_char *= 10;
      x->composed_char += keycode - 0x4e;
      if (x->composed_char > 255)
        return FNU_ERRKEY;
      return FNU_NOKEY;
    case 0x52:
      x->composed_char *= 10;
      if (x->composed_char > 255)
        return FNU_ERRKEY;
      return FNU_NOKEY;

    case 0xc7: case 0xc8: case 0xc9:
    case 0xcb: case 0xcc: case 0xcd:
    case 0xcf: case 0xd0: case 0xd1:
    case 0xd2:
      return FNU_NOKEY;

    case 0x38:
      break;

    default:
      if (x->composed_char > 0) {
        x->flags &= ~COMPOSE;
        x->composed_char = 0;
        return FNU_ERRKEY;
      }
      break;
    }
  }

  action = (uint)genkbd_keyaction(kbd, keycode, scancode & 0x80, &x->state, &x->accents);
  return action;
}

void genkbd_clear_state(struct kbdxlate *x) {
  x->flags = 0;
  x->state &= FNU_KB_LOCKS;
  x->accents = 0;
  x->composed_char = 0;
}

int genkbd_modeioctl(keyboard_t *kbd, struct kbdxlate *x, uint64_t cmd, void *arg) {
  switch (cmd) {
  case FNU_KBGETMODE:
    *(int *)arg = x->mode;
    return 0;
  case FNU_KBSETMODE:
    switch (*(int *)arg) {
    case FNU_KB_XLATE:
      if (x->mode != FNU_KB_XLATE) {
        x->state &= ~FNU_KB_LOCKS;
        x->state |= KBD_LED_VAL(kbd);
      }
      // fallthrough
    case FNU_KB_RAW:
    case FNU_KB_CODE:
      if (x->mode != *(int *)arg) {
        kbdd_clear_state(kbd);
        x->mode = *(int *)arg;
      }
      return 0;
    default:
      return FNU_ERR_INVALID;
    }
  case FNU_KBGETLEDS:
    *(int *)arg = KBD_LED_VAL(kbd);
    return 0;
  case FNU_KBGETLOCKS:
    *(int *)arg = x->state & FNU_KB_LOCKS;
    return 0;
  case FNU_KBSETLOCKS:
    if (*(int *)arg & ~FNU_KB_LOCKS)
      return FNU_ERR_INVALID;
    x->state &= ~FNU_KB_LOCKS;
    x->state |= *(int *)arg;
    return kbdd_ioctl(kbd, FNU_KBSETLEDS, arg);
  case FNU_KBSETMAP:
  case FNU_KBSETKEY:
  case FNU_KBSETACCENTS:
    x->accents = 0;
    // fallthrough
  default:
    return genkbd_commonioctl(kbd, cmd, arg);
  }
}
