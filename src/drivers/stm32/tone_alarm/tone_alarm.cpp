/****************************************************************************
 *
 *   Copyright (C) 2013 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * Driver for the PX4 audio alarm port, /dev/tone_alarm.
 *
 * The tone_alarm driver supports a set of predefined "alarm"
 * tunes and one user-supplied tune.
 *
 * The TONE_SET_ALARM ioctl can be used to select a predefined
 * alarm tune, from 1 - <TBD>.  Selecting tune zero silences
 * the alarm.
 *
 * Tunes follow the syntax of the Microsoft GWBasic/QBasic PLAY
 * statement, with some exceptions and extensions.
 *
 * From Wikibooks:
 *
 * PLAY "[string expression]"
 * 
 * Used to play notes and a score ... The tones are indicated by letters A through G.
 * Accidentals are indicated with a "+" or "#" (for sharp) or "-" (for flat) 
 * immediately after the note letter. See this example:
 * 
 *   PLAY "C C# C C#"
 *
 * Whitespaces are ignored inside the string expression. There are also codes that
 * set the duration, octave and tempo. They are all case-insensitive. PLAY executes 
 * the commands or notes the order in which they appear in the string. Any indicators 
 * that change the properties are effective for the notes following that indicator.
 *
 * Ln     Sets the duration (length) of the notes. The variable n does not indicate an actual duration
 *        amount but rather a note type; L1 - whole note, L2 - half note, L4 - quarter note, etc.
 *        (L8, L16, L32, L64, ...). By default, n = 4.
 *        For triplets and quintets, use L3, L6, L12, ... and L5, L10, L20, ... series respectively.
 *        The shorthand notation of length is also provided for a note. For example, "L4 CDE L8 FG L4 AB"
 *        can be shortened to "L4 CDE F8G8 AB". F and G play as eighth notes while others play as quarter notes.
 * On     Sets the current octave. Valid values for n are 0 through 6. An octave begins with C and ends with B.
 *        Remember that C- is equivalent to B. 
 * < >    Changes the current octave respectively down or up one level.
 * Nn     Plays a specified note in the seven-octave range. Valid values are from 0 to 84. (0 is a pause.)
 *        Cannot use with sharp and flat. Cannot use with the shorthand notation neither.
 * MN     Stand for Music Normal. Note duration is 7/8ths of the length indicated by Ln. It is the default mode.
 * ML     Stand for Music Legato. Note duration is full length of that indicated by Ln.
 * MS     Stand for Music Staccato. Note duration is 3/4ths of the length indicated by Ln.
 * Pn     Causes a silence (pause) for the length of note indicated (same as Ln). 
 * Tn     Sets the number of "L4"s per minute (tempo). Valid values are from 32 to 255. The default value is T120. 
 * .      When placed after a note, it causes the duration of the note to be 3/2 of the set duration.
 *        This is how to get "dotted" notes. "L4 C#." would play C sharp as a dotted quarter note.
 *        It can be used for a pause as well.
 *
 * Extensions/variations:
 *
 * MB MF  The MF command causes the tune to play once and then stop. The MB command causes the
 *        tune to repeat when it ends.
 *
 */

#include <nuttx/config.h>
#include <debug.h>

#include <drivers/device/device.h>
#include <drivers/drv_tone_alarm.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <math.h>
#include <ctype.h>

#include <board_config.h>
#include <drivers/drv_hrt.h>

#include <arch/stm32/chip.h>
#include <up_internal.h>
#include <up_arch.h>

#include <stm32.h>
#include <stm32_gpio.h>
#include <stm32_tim.h>

#include <systemlib/err.h>

/* Tone alarm configuration */
#if   TONE_ALARM_TIMER == 2
# define TONE_ALARM_BASE		STM32_TIM2_BASE
# define TONE_ALARM_CLOCK		STM32_APB1_TIM2_CLKIN
# define TONE_ALARM_CLOCK_ENABLE	RCC_APB1ENR_TIM2EN
# ifdef CONFIG_STM32_TIM2
#  error Must not set CONFIG_STM32_TIM2 when TONE_ALARM_TIMER is 2
# endif
#elif TONE_ALARM_TIMER == 3
# define TONE_ALARM_BASE		STM32_TIM3_BASE
# define TONE_ALARM_CLOCK		STM32_APB1_TIM3_CLKIN
# define TONE_ALARM_CLOCK_ENABLE	RCC_APB1ENR_TIM3EN
# ifdef CONFIG_STM32_TIM3
#  error Must not set CONFIG_STM32_TIM3 when TONE_ALARM_TIMER is 3
# endif
#elif TONE_ALARM_TIMER == 4
# define TONE_ALARM_BASE		STM32_TIM4_BASE
# define TONE_ALARM_CLOCK		STM32_APB1_TIM4_CLKIN
# define TONE_ALARM_CLOCK_ENABLE	RCC_APB1ENR_TIM4EN
# ifdef CONFIG_STM32_TIM4
#  error Must not set CONFIG_STM32_TIM4 when TONE_ALARM_TIMER is 4
# endif
#elif TONE_ALARM_TIMER == 5
# define TONE_ALARM_BASE		STM32_TIM5_BASE
# define TONE_ALARM_CLOCK		STM32_APB1_TIM5_CLKIN
# define TONE_ALARM_CLOCK_ENABLE	RCC_APB1ENR_TIM5EN
# ifdef CONFIG_STM32_TIM5
#  error Must not set CONFIG_STM32_TIM5 when TONE_ALARM_TIMER is 5
# endif
#elif TONE_ALARM_TIMER == 9
# define TONE_ALARM_BASE		STM32_TIM9_BASE
# define TONE_ALARM_CLOCK		STM32_APB1_TIM9_CLKIN
# define TONE_ALARM_CLOCK_ENABLE	RCC_APB1ENR_TIM9EN
# ifdef CONFIG_STM32_TIM9
#  error Must not set CONFIG_STM32_TIM9 when TONE_ALARM_TIMER is 9
# endif
#elif TONE_ALARM_TIMER == 10
# define TONE_ALARM_BASE		STM32_TIM10_BASE
# define TONE_ALARM_CLOCK		STM32_APB1_TIM10_CLKIN
# define TONE_ALARM_CLOCK_ENABLE	RCC_APB1ENR_TIM10EN
# ifdef CONFIG_STM32_TIM10
#  error Must not set CONFIG_STM32_TIM10 when TONE_ALARM_TIMER is 10
# endif
#elif TONE_ALARM_TIMER == 11
# define TONE_ALARM_BASE		STM32_TIM11_BASE
# define TONE_ALARM_CLOCK		STM32_APB1_TIM11_CLKIN
# define TONE_ALARM_CLOCK_ENABLE	RCC_APB1ENR_TIM11EN
# ifdef CONFIG_STM32_TIM11
#  error Must not set CONFIG_STM32_TIM11 when TONE_ALARM_TIMER is 11
# endif
#else
# error Must set TONE_ALARM_TIMER to a generic timer in order to use this driver.
#endif

#if TONE_ALARM_CHANNEL == 1
# define TONE_CCMR1	(3 << 4)
# define TONE_CCMR2	0
# define TONE_CCER	(1 << 0)
# define TONE_rCCR	rCCR1
#elif TONE_ALARM_CHANNEL == 2
# define TONE_CCMR1	(3 << 12)
# define TONE_CCMR2	0
# define TONE_CCER	(1 << 4)
# define TONE_rCCR	rCCR2
#elif TONE_ALARM_CHANNEL == 3
# define TONE_CCMR1	0
# define TONE_CCMR2	(3 << 4)
# define TONE_CCER	(1 << 8)
# define TONE_rCCR	rCCR3
#elif TONE_ALARM_CHANNEL == 4
# define TONE_CCMR1	0
# define TONE_CCMR2	(3 << 12)
# define TONE_CCER	(1 << 12)
# define TONE_rCCR	rCCR4
#else
# error Must set TONE_ALARM_CHANNEL to a value between 1 and 4 to use this driver.
#endif


/*
 * Timer register accessors
 */
#define REG(_reg)	(*(volatile uint32_t *)(TONE_ALARM_BASE + _reg))

#define rCR1     	REG(STM32_GTIM_CR1_OFFSET)
#define rCR2     	REG(STM32_GTIM_CR2_OFFSET)
#define rSMCR    	REG(STM32_GTIM_SMCR_OFFSET)
#define rDIER    	REG(STM32_GTIM_DIER_OFFSET)
#define rSR      	REG(STM32_GTIM_SR_OFFSET)
#define rEGR     	REG(STM32_GTIM_EGR_OFFSET)
#define rCCMR1   	REG(STM32_GTIM_CCMR1_OFFSET)
#define rCCMR2   	REG(STM32_GTIM_CCMR2_OFFSET)
#define rCCER    	REG(STM32_GTIM_CCER_OFFSET)
#define rCNT     	REG(STM32_GTIM_CNT_OFFSET)
#define rPSC     	REG(STM32_GTIM_PSC_OFFSET)
#define rARR     	REG(STM32_GTIM_ARR_OFFSET)
#define rCCR1    	REG(STM32_GTIM_CCR1_OFFSET)
#define rCCR2    	REG(STM32_GTIM_CCR2_OFFSET)
#define rCCR3    	REG(STM32_GTIM_CCR3_OFFSET)
#define rCCR4    	REG(STM32_GTIM_CCR4_OFFSET)
#define rDCR     	REG(STM32_GTIM_DCR_OFFSET)
#define rDMAR    	REG(STM32_GTIM_DMAR_OFFSET)

class ToneAlarm : public device::CDev
{
public:
	ToneAlarm();
	~ToneAlarm();

	virtual int		init();

	virtual int		ioctl(file *filp, int cmd, unsigned long arg);
	virtual ssize_t		write(file *filp, const char *buffer, size_t len);

private:
	static const unsigned	_tune_max = 1024; // be reasonable about user tunes
	static const char	* const _default_tunes[];
	static const unsigned	_default_ntunes;
	static const uint8_t	_note_tab[];

	unsigned		_default_tune_number; // number of currently playing default tune (0 for none)

	const char		*_user_tune;

	const char		*_tune;		// current tune string
	const char		*_next;		// next note in the string

	unsigned		_tempo;
	unsigned		_note_length;
	enum { MODE_NORMAL, MODE_LEGATO, MODE_STACCATO} _note_mode;
	unsigned		_octave;
	unsigned		_silence_length; // if nonzero, silence before next note
	bool			_repeat;	// if true, tune restarts at end

	hrt_call		_note_call;	// HRT callout for note completion

	// Convert a note value in the range C1 to B7 into a divisor for
	// the configured timer's clock.
	//
	unsigned		note_to_divisor(unsigned note);

	// Calculate the duration in microseconds of play and silence for a 
	// note given the current tempo, length and mode and the number of 
	// dots following in the play string.
	//
	unsigned		note_duration(unsigned &silence, unsigned note_length, unsigned dots);

	// Calculate the duration in microseconds of a rest corresponding to
	// a given note length.
	//
	unsigned		rest_duration(unsigned rest_length, unsigned dots);

	// Start playing the note
	//
	void			start_note(unsigned note);

	// Stop playing the current note and make the player 'safe'
	//
	void			stop_note();

	// Start playing the tune
	//
	void			start_tune(const char *tune);

	// Parse the next note out of the string and play it
	//
	void			next_note();

	// Find the next character in the string, discard any whitespace and
	// return the canonical (uppercase) version.
	//
	int			next_char();

	// Extract a number from the string, consuming all the digit characters.
	//
	unsigned		next_number();

	// Consume dot characters from the string, returning the number consumed.
	//
	unsigned		next_dots();

	// hrt_call trampoline for next_note
	//
	static void		next_trampoline(void *arg);

};

// predefined tune array
const char * const ToneAlarm::_default_tunes[] = {
	"MFT240L8 O4aO5dc O4aO5dc O4aO5dc L16dcdcdcdc",		// startup tune
	"MBT200a8a8a8PaaaP",					// ERROR tone
	"MFT200e8a8a",						// NotifyPositive tone
	"MFT200e8e",						// NotifyNeutral tone
	"MFT200e8c8e8c8e8c8",					// NotifyNegative tone
	"MFT90O3C16.C32C16.C32C16.C32G16.E32G16.E32G16.E32C16.C32C16.C32C16.C32G16.E32G16.E32G16.E32C4", // charge!
	"MFT60O3C32O2A32F16F16F32G32A32A+32O3C16C16C16O2A16",	// dixie
	"MFT90O2C16C16C16F8.A8C16C16C16F8.A4P16P8",		// cucuracha
	"MNT150L8O2GGABGBADGGABL4GL8F+",			// yankee
	"MFT200O3C4.O2A4.G4.F4.D8E8F8D4F8C2.O2G4.O3C4.O2A4.F4.D8E8F8G4A8G2P8", // daisy
	"T200O2B4P8B16B16B4P8B16B16B8G+8E8G+8B8G+8B8O3E8"	// william tell
	"O2B8G+8E8G+8B8G+8B8O3E8O2B4P8B16B16B4P8B16"
	"O2B16B4P8B16B16B4P8B16B16B8B16B16B8B8B8B16"
	"O2B16B8B8B8B16B16B8B8B8B16B16B8B8B2B2B8P8"
	"P4P4P8O1B16B16B8B16B16B8B16B16O2E8F+8G+8"
	"O1B16B16B8B16B16O2E8G+16G+16F+8D+8O1B8B16"
	"O1B16B8B16B16B8B16B16O2E8F+8G+8E16G+16B4"
	"O2B16A16G+16F+16E8G+8E8O3B16B16B8B16B16B8"
	"O3B16B16O4E8F+8G+8O3B16B16B8B16B16O4E8G+16"
	"O4G+16F+8D+8O3B8B16B16B8B16B16B8B16B16O4E8"
	"O4F+8G+8E16G+16B4B16A16G+16F+16E8G+8E8O3G+16"
	"O3G+16G+8G+16G+16G+8G+16G+16G+8O4C+8O3G+8"
	"O4C+8O3G+8O4C+8O3G+8F+8E8D+8C+8G+16G+16G+8"
	"O3G+16G+16G+8G+16G+16G+8O4C+8O3G+8O4C+8O3G+8"
	"O4C+8O3B8A+8B8A+8B8G+16G+16G+8G+16G+16G+8"
	"O3G+16G+16G+8O4C+8O3G+8O4C+8O3G+8O4C+8O3G+8"
	"O3F+8E8D+8C+8G+16G+16G+8G+16G+16G+8G+16G+16"
	"O3G+8O4C+8O3G+8O4C+8O3G+8O4C+8O3B8A+8B8O2B16"
	"O2B16B8F+16F+16F+8F+16F+16F+8G+8A8F+4A8G+8"
	"O2E4G+8F+8F+8F+8O3F+16F+16F+8F+16F+16F+8"
	"O3G+8A8F+4A8G+8E4G+8F+8O2B16B16B8O1B16B16"
	"O1B8B16B16B8B16B16O2E8F+8G+8O1B16B16B8B16"
	"O1B16O2E8G+16G+16F+8D+8O1B8B16B16B8B16B16"
	"O1B8B16B16O2E8F+8G+8E16G+16B4B16A16G+16F+16"
	"O2E8G+8E8O3B16B16B8B16B16B8B16B16O4E8F+8"
	"O4G+8O3B16B16B8B16B16O4E8G+16G+16F+8D+8O3B8"
	"O3B16B16B8B16B16B8B16B16O4E8F+8G+8E16G+16"
	"O4B4B16A16G+16F+16E8G+8E8O3E64F64G64A64B64"
	"O4C64D64E8E16E16E8E8G+4.F+8E8D+8E8C+8O3B16"
	"O4C+16O3B16O4C+16O3B16O4C+16D+16E16O3A16"
	"O3B16A16B16A16B16O4C+16D+16O3G+16A16G+16"
	"O3A16G+16A16B16O4C+16O3F+16G+16F+16G+16F+16"
	"O3G+16F+16G+16F+16G+16F+16D+16O2B16O3B16"
	"O4C+16D+16E8D+8E8C+8O3B16O4C+16O3B16O4C+16"
	"O3B16O4C+16D+16E16O3A16B16A16B16A16B16O4C+16"
	"O4D+16O3G+16A16G+16A16G+16A16B16O4C+16O3F+16"
	"O3G+16F+16G+16F+16A16F+16E16E8P8C+4C+16O2C16"
	"O3C+16O2C16O3D+16C+16O2B16A16A16G+16E16C+16"
	"O2C+16C+16C+16C+16E16D+16O1C16G+16G+16G+16"
	"O1G+16G+16G+16O2C+16E16G+16O3C+16C+16C+16"
	"O3C+16C+16O2C16O3C+16O2C16O3D+16C+16O2B16"
	"O2A16A16G+16E16C+16C+16C+16C+16C+16E16D+16"
	"O1C16G+16G+16G+16G+16G+16G+16O2C+16E16G+16"
	"O3C+16E16D+16C+16D+16O2C16G+16G+16G+16O3G+16"
	"O3E16C+16D+16O2C16G+16G+16G+16O3G+16E16C+16"
	"O3D+16O2B16G+16G+16A+16G16D+16D+16G+16G16"
	"O2G+16G16G+16A16G+16F+16E16O1B16A+16B16O2E16"
	"O1B16O2F+16O1B16O2G+16E16D+16E16G+16E16A16"
	"O2F+16B16O3G+16F+16E16D+16F+16E16C+16O2B16"
	"O3C+16O2B16O3C+16D+16E16F+16G+16O2A16B16"
	"O2A16B16O3C+16D+16E16F+16O2G+16A16G+16A16"
	"O2C16O3C+16D+16E16O2F+16G+16F+16G+16F+16"
	"O2G+16F+16G+16F+16G+16F+16D+16O1B16C16O2C+16"
	"O2D+16E16O1B16A+16B16O2E16O1B16O2F+16O1B16"
	"O2G+16E16D+16E16G+16E16A16F+16B16O3G+16F+16"
	"O3E16D+16F+16E16C+16O2B16O3C+16O2B16O3C+16"
	"O3D+16E16F+16G+16O2A16B16A16B16O3C+16D+16"
	"O3E16F+16O2G+16A16G+16A16B16O3C+16D+16E16"
	"O2F+16O3C+16O2C16O3C+16D+16C+16O2A16F+16"
	"O2E16O3E16F+16G+16A16B16O4C+16D+16E8E16E16"
	"O4E8E8G+4.F8E8D+8E8C+8O3B16O4C+16O3B16O4C+16"
	"O3B16O4C+16D+16E16O3A16B16A16B16A16B16O4C+16"
	"O4D+16O3G+16A16G+16A16G+16A16B16O4C+16O3F+16"
	"O3G+16F+16G+16F+16G+16F+16G+16F+16G+16F+16"
	"O3D+16O2B16O3B16O4C+16D+16E8E16E16E8E8G+4."
	"O4F+8E8D+8E8C+8O3B16O4C+16O3B16O4C+16O3B16"
	"O4C+16D+16E16O3A16B16A16B16A16B16O4C+16D+16"
	"O3G+16A16G+16A16G+16A16B16O4C+16O3F+16G+16"
	"O3F+16G+16F+16A16G+16F+16E8O2B8O3E8G+16G+16"
	"O3G+8G+16G+16G+8G+16G+16G+8O4C+8O3G+8O4C+8"
	"O3G+8O4C+8O3G+8F+8E8D+8C+8G+16G+16G+8G+16"
	"O3G+16G+8G+16G+16G+8O4C+8O3G+8O4C+8O3G+8"
	"O4C+8O3B8A+8B8A+8B8G+16G+16G+8G+16G+16G+8"
	"O3G+16G+16G+8O4C+8O3G+8O4C+8O3G+8O4C+8O3G+8"
	"O3F+8E8D+8C+8G+16G+16G+8G+16G+16G+8G+16G+16"
	"O3G+8O4C+8O3G+8O4C+8O3G+8O4C+8O3B8A+8B8A+8"
	"O3B8O2F+16F+16F+8F+16F+16F+8G+8A8F+4A8G+8"
	"O2E4G+8F+8B8O1B8O2F+16F+16F+8F+16F+16F+8"
	"O2G+8A8F+4A8G+8E4G+8F+8B16B16B8O1B16B16B8"
	"O1B16B16B8B16B16O2E8F+8G+8O1B16B16B8B16B16"
	"O2E8G+16G+16F+8D+8O1B8B16B16B8B16B16B8B16"
	"O1B16O2E8F+8G+8E16G+16B4B16A16G+16F+16E8"
	"O1B8O2E8O3B16B16B8B16B16B8B16B16O4E8F+8G+8"
	"O3B16B16B8B16B16O4E8G+16G+16F+8D+8O3B8B16"
	"O3B16B8B16B16B8B16B16O4E8F+8G+8O3E16G+16"
	"O3B4B16A16G+16F+16E16F+16G+16A16G+16A16B16"
	"O4C+16O3B16O4C+16D+16E16D+16E16F+16G+16A16"
	"O3B16O4A16O3B16O4A16O3B16O4A16O3B16O4A16"
	"O3B16O4A16O3B16O4A16O3B16O4A16O3B16E16F+16"
	"O3G+16A16G+16A16B16O4C+16O3B16O4C+16D+16"
	"O4E16D+16E16F+16G+16A16O3B16O4A16O3B16O4A16"
	"O3B16O4A16O3B16O4A16O3B16O4A16O3B16O4A16"
	"O3B16O4A16O3B16P16G+16O4G+16O3G+16P16D+16"
	"O4D+16O3D+16P16E16O4E16O3E16P16A16O4A16O3A16"
	"P16O3G+16O4G+16O3G+16P16D+16O4D+16O3D+16"
	"P16O3E16O4E16O3E16P16A16O4A16O3A16O4G16O3G16"
	"O4G16O3G16O4G16O3G16O4G16O3G16O4G8E8C8E8"
	"O4G+16O3G+16O4G+16O3G+16O4G+16O3G+16O4G+16"
	"O3G+16O4G+8E8O3B8O4E8G+16O3G+16O4G+16O3G+16"
	"O4G+16O3G+16O4G+16O3G+16O4G+8F8C+8F8A+16"
	"O3A+16O4A+16O3A+16O4A+16O3A+16O4A+16O3A+16"
	"O4A+8G8E8G8B8P16A+16P16A16P16G+16P16F+16"
	"P16O4E16P16D+16P16C+16P16O3B16P16A+16P16"
	"O3A16P16G+16P16F+16P16E16P16D+16P16F+16E16"
	"O3F+16G+16A16G+16A16B16O4C+16O3B16O4C+16"
	"O4D+16E16D+16E16F+16G+16A16O3B16O4A16O3B16"
	"O4A16O3B16O4A16O3B16O4A16O3B16O4A16O3B16"
	"O4A16O3B16O4A16O3B16E16F+16G+16A16G+16A16"
	"O3B16O4C+16O3B16O4C+16D+16E16D+16E16F+16"
	"O4G+16A16O3B16O4A16O3B16O4A16O3B16O4A16O3B16"
	"O4A16O3B16O4A16O3B16O4A16O3B16O4A16O3B16"
	"P16O3G+16O4G+16O3G+16P16D+16O4D+16O3D+16"
	"P16O3E16O4E16O3E16P16A16O4A16O3A16P16G+16"
	"O4G+16O3G+16P16D+16O4D+16O3D+16P16E16O4E16"
	"O3E16P16A16O4A16O3A16O4G16O3G16O4G16O3G16"
	"O4G16O3G16O4G16O3G16O4G8E8C8E8G+16O3G+16"
	"O4G+16O3G+16O4G+16O3G+16O4G+16O3G+16O4G+8"
	"O4E8O3B8O4E8G+16O3G+16O4G+16O3G+16O4G+16"
	"O3G+16O4G+16O3G+16O4G+8F8C+8F8A+16O3A+16"
	"O4A+16O3A+16O4A+16O3A+16O4A+16O3A+16O4A+8"
	"O4G8E8G8B8P16A+16P16A16P16G+16P16F+16P16"
	"O4E16P16D+16P16C+16P16O3B16P16A+16P16A16"
	"P16O3G+16P16F+16P16E16P16D+16P16F16E16D+16"
	"O3E16D+16E8B16B16B8B16B16B8B16B16O4E8F+8"
	"O4G+8O3B16B16B8B16B16B8B16B16O4G+8A8B8P8"
	"O4E8F+8G+8P8O3G+8A8B8P8P2O2B16C16O3C+16D16"
	"O3D+16E16F16F+16G16G+16A16A+16B16C16O4C+16"
	"O4D+16E16D+16F+16D+16E16D+16F+16D+16E16D+16"
	"O4F+16D+16E16D+16F+16D+16E16D+16F+16D+16"
	"O4E16D+16F+16D+16E16D+16F+16D+16E16D+16F+16"
	"O4D+16E8E16O3E16O4E16O3E16O4E16O3E16O4E8"
	"O3B16O2B16O3B16O2B16O3B16O2B16O3B8G+16O2G+16"
	"O3G+16O2G+16O3G+16O2G+16O3G8E16O2E16O3E16"
	"O2E16O3E16O2E16O3E8E16E16E8E8E8O2B16B16B8"
	"O2B8B8G+16G+16G+8G+8G+8E16E16E8E8E8O1B8O2E8"
	"O1B8O2G+8E8B8G+8O3E8O2B8O3E8O2B8O3G+8E8B8"
	"O3G+8O4E4P8E16E16E8E8E8E8E4P8E16E4P8O2E16"
	"O2E2P64",
	"MNT75L1O2G",					//arming warning
	"MBNT100a8",					//battery warning slow
	"MBNT255a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8a8"	//battery warning fast // XXX why is there a break before a repetition
};

const unsigned ToneAlarm::_default_ntunes = sizeof(_default_tunes) / sizeof(_default_tunes[0]);

// semitone offsets from C for the characters 'A'-'G'
const uint8_t ToneAlarm::_note_tab[] = {9, 11, 0, 2, 4, 5, 7};

/*
 * Driver 'main' command.
 */
extern "C" __EXPORT int tone_alarm_main(int argc, char *argv[]);


ToneAlarm::ToneAlarm() :
	CDev("tone_alarm", "/dev/tone_alarm"),
	_default_tune_number(0),
	_user_tune(nullptr),
	_tune(nullptr),
	_next(nullptr)
{
	// enable debug() calls
	//_debug_enabled = true;
}

ToneAlarm::~ToneAlarm()
{
}

int
ToneAlarm::init()
{
	int ret;

	ret = CDev::init();

	if (ret != OK)
		return ret;

	/* configure the GPIO to the idle state */
	stm32_configgpio(GPIO_TONE_ALARM_IDLE);

	/* clock/power on our timer */
	modifyreg32(STM32_RCC_APB1ENR, 0, TONE_ALARM_CLOCK_ENABLE);

	/* initialise the timer */
	rCR1 = 0;
	rCR2 = 0;
	rSMCR = 0;
	rDIER = 0;
	rCCER &= TONE_CCER;		/* unlock CCMR* registers */
	rCCMR1 = TONE_CCMR1;
	rCCMR2 = TONE_CCMR2;
	rCCER = TONE_CCER;
	rDCR = 0;

	/* toggle the CC output each time the count passes 1 */
	TONE_rCCR = 1;

	/* default the timer to a prescale value of 1; playing notes will change this */
	rPSC = 0;

	/* make sure the timer is running */
	rCR1 = GTIM_CR1_CEN;

	debug("ready");
	return OK;
}

unsigned
ToneAlarm::note_to_divisor(unsigned note)
{
	// compute the frequency first (Hz)
	float freq = 880.0f * expf(logf(2.0f) * ((int)note - 46) / 12.0f);

	float period = 0.5f / freq;

	// and the divisor, rounded to the nearest integer
	unsigned divisor = (period * TONE_ALARM_CLOCK) + 0.5f;

	return divisor;
}

unsigned
ToneAlarm::note_duration(unsigned &silence, unsigned note_length, unsigned dots)
{
	unsigned whole_note_period = (60 * 1000000 * 4) / _tempo;

	if (note_length == 0)
		note_length = 1;
	unsigned note_period = whole_note_period / note_length;

	switch (_note_mode) {
	case MODE_NORMAL:
		silence = note_period / 8;
		break;
	case MODE_STACCATO:
		silence = note_period / 4;
		break;
	default:
	case MODE_LEGATO:
		silence = 0;
		break;
	}
	note_period -= silence;

	unsigned dot_extension = note_period / 2;
	while (dots--) {
		note_period += dot_extension;
		dot_extension /= 2;
	}

	return note_period;
}

unsigned
ToneAlarm::rest_duration(unsigned rest_length, unsigned dots)
{
	unsigned whole_note_period = (60 * 1000000 * 4) / _tempo;

	if (rest_length == 0)
		rest_length = 1;

	unsigned rest_period = whole_note_period / rest_length;

	unsigned dot_extension = rest_period / 2;
	while (dots--) {
		rest_period += dot_extension;
		dot_extension /= 2;
	}

	return rest_period;
}

void
ToneAlarm::start_note(unsigned note)
{
	// compute the divisor
	unsigned divisor = note_to_divisor(note);

	// pick the lowest prescaler value that we can use
	// (note that the effective prescale value is 1 greater)
	unsigned prescale = divisor / 65536;

	// calculate the timer period for the selected prescaler value
	unsigned period = (divisor / (prescale + 1)) - 1;

	rPSC = prescale;	// load new prescaler
	rARR = period;		// load new toggle period
	rEGR = GTIM_EGR_UG;	// force a reload of the period
	rCCER |= TONE_CCER;	// enable the output

	// configure the GPIO to enable timer output
	stm32_configgpio(GPIO_TONE_ALARM);
}

void
ToneAlarm::stop_note()
{
	/* stop the current note */
	rCCER &= ~TONE_CCER;

	/*
	 * Make sure the GPIO is not driving the speaker.
	 */
	stm32_configgpio(GPIO_TONE_ALARM_IDLE);
}

void
ToneAlarm::start_tune(const char *tune)
{
	// kill any current playback
	hrt_cancel(&_note_call);

	// record the tune
	_tune = tune;
	_next = tune;

	// initialise player state
	_tempo = 120;
	_note_length = 4;
	_note_mode = MODE_NORMAL;
	_octave = 4;
	_silence_length = 0;
	_repeat = false;		// otherwise command-line tunes repeat forever...

	// schedule a callback to start playing
	hrt_call_after(&_note_call, 0, (hrt_callout)next_trampoline, this);
}

void
ToneAlarm::next_note()
{
	// do we have an inter-note gap to wait for?
	if (_silence_length > 0) {
		stop_note();
		hrt_call_after(&_note_call, (hrt_abstime)_silence_length, (hrt_callout)next_trampoline, this);
		_silence_length = 0;
		return;
	}

	// make sure we still have a tune - may be removed by the write / ioctl handler
	if ((_next == nullptr) || (_tune == nullptr)) {
		stop_note();
		return;
	}

	// parse characters out of the string until we have resolved a note
	unsigned note = 0;
	unsigned note_length = _note_length;
	unsigned duration;

	while (note == 0) {
		// we always need at least one character from the string
		int c = next_char();
		if (c == 0)
			goto tune_end;
		_next++;

		switch (c) {
		case 'L':	// select note length
			_note_length = next_number();
			if (_note_length < 1)
				goto tune_error;
			break;

		case 'O':	// select octave
			_octave = next_number();
			if (_octave > 6)
				_octave = 6;
			break;

		case '<':	// decrease octave
			if (_octave > 0)
				_octave--;
			break;

		case '>':	// increase octave
			if (_octave < 6)
				_octave++;
			break;

		case 'M':	// select inter-note gap
			c = next_char();
			if (c == 0)
				goto tune_error;
			_next++;
			switch (c) {
			case 'N':
				_note_mode = MODE_NORMAL;
				break;
			case 'L':
				_note_mode = MODE_LEGATO;
				break;
			case 'S':
				_note_mode = MODE_STACCATO;
				break;
			case 'F':
				_repeat = false;
				break;
			case 'B':
				_repeat = true;
				break;
			default:
				goto tune_error;
			}
			break;

		case 'P':	// pause for a note length
			stop_note();
			hrt_call_after(&_note_call, 
				(hrt_abstime)rest_duration(next_number(), next_dots()),
				(hrt_callout)next_trampoline, 
				this);
			return;

		case 'T': {	// change tempo
			unsigned nt = next_number();

			if ((nt >= 32) && (nt <= 255)) {
				_tempo = nt;
			} else {
				goto tune_error;
			}
			break;
		}

		case 'N':	// play an arbitrary note
			note = next_number();
			if (note > 84)
				goto tune_error;
			if (note == 0) {
				// this is a rest - pause for the current note length
				hrt_call_after(&_note_call,
					(hrt_abstime)rest_duration(_note_length, next_dots()),
					(hrt_callout)next_trampoline, 
					this);
				return;				
			}
			break;

		case 'A'...'G':	// play a note in the current octave
			note = _note_tab[c - 'A'] + (_octave * 12) + 1;
			c = next_char();
			switch (c) {
			case '#':	// up a semitone
			case '+':
				if (note < 84)
					note++;
				_next++;
				break;
			case '-':	// down a semitone
				if (note > 1)
					note--;
				_next++;
				break;
			default:
				// 0 / no next char here is OK
				break;
			}
			// shorthand length notation
			note_length = next_number();
			if (note_length == 0)
				note_length = _note_length;
			break;

		default:
			goto tune_error;
		}
	}

	// compute the duration of the note and the following silence (if any)
	duration = note_duration(_silence_length, note_length, next_dots());

	// start playing the note
	start_note(note);

	// and arrange a callback when the note should stop
	hrt_call_after(&_note_call, (hrt_abstime)duration, (hrt_callout)next_trampoline, this);
	return;

	// tune looks bad (unexpected EOF, bad character, etc.)
tune_error:
	lowsyslog("tune error\n");
	_repeat = false;		// don't loop on error

	// stop (and potentially restart) the tune
tune_end:
	stop_note();
	if (_repeat) {
		start_tune(_tune);
	} else {
		_tune = nullptr;
		_default_tune_number = 0;
	}
	return;
}

int
ToneAlarm::next_char()
{
	while (isspace(*_next)) {
		_next++;
	}
	return toupper(*_next);
}

unsigned
ToneAlarm::next_number()
{
	unsigned number = 0;
	int c;

	for (;;) {
		c = next_char();
		if (!isdigit(c))
			return number;
		_next++;
		number = (number * 10) + (c - '0');
	}
}

unsigned
ToneAlarm::next_dots()
{
	unsigned dots = 0;

	while (next_char() == '.') {
		_next++;
		dots++;
	}
	return dots;
}

void
ToneAlarm::next_trampoline(void *arg)
{
	ToneAlarm *ta = (ToneAlarm *)arg;

	ta->next_note();
}


int
ToneAlarm::ioctl(file *filp, int cmd, unsigned long arg)
{
	int result = OK;

	debug("ioctl %i %u", cmd, arg);

//	irqstate_t flags = irqsave();

	/* decide whether to increase the alarm level to cmd or leave it alone */
	switch (cmd) {
	case TONE_SET_ALARM:
		debug("TONE_SET_ALARM %u", arg);

		if (arg <= _default_ntunes) {
			if (arg == 0) {
				// stop the tune
				_tune = nullptr;
				_next = nullptr;
			} else {
				/* always interrupt alarms, unless they are repeating and already playing */
				if (!(_repeat && _default_tune_number == arg)) {
					/* play the selected tune */
					_default_tune_number = arg;
					start_tune(_default_tunes[arg - 1]);
				}
			}
		} else {
			result = -EINVAL;
		}

		break;

	default:
		result = -ENOTTY;
		break;
	}

//	irqrestore(flags);

	/* give it to the superclass if we didn't like it */
	if (result == -ENOTTY)
		result = CDev::ioctl(filp, cmd, arg);

	return result;
}

int
ToneAlarm::write(file *filp, const char *buffer, size_t len)
{
	// sanity-check the buffer for length and nul-termination
	if (len > _tune_max)
		return -EFBIG;

	// if we have an existing user tune, free it
	if (_user_tune != nullptr) {

		// if we are playing the user tune, stop
		if (_tune == _user_tune) {
			_tune = nullptr;
			_next = nullptr;
		}

		// free the old user tune
		free((void *)_user_tune);
		_user_tune = nullptr;
	}

	// if the new tune is empty, we're done
	if (buffer[0] == '\0')
		return OK;

	// allocate a copy of the new tune
	_user_tune = strndup(buffer, len);
	if (_user_tune == nullptr)
		return -ENOMEM;

	// and play it
	start_tune(_user_tune);

	return len;
}

/**
 * Local functions in support of the shell command.
 */
namespace
{

ToneAlarm	*g_dev;

int
play_tune(unsigned tune)
{
	int	fd, ret;

	fd = open("/dev/tone_alarm", 0);

	if (fd < 0)
		err(1, "/dev/tone_alarm");

	ret = ioctl(fd, TONE_SET_ALARM, tune);
	close(fd);

	if (ret != 0)
		err(1, "TONE_SET_ALARM");

	exit(0);
}

int
play_string(const char *str)
{
	int	fd, ret;

	fd = open("/dev/tone_alarm", O_WRONLY);

	if (fd < 0)
		err(1, "/dev/tone_alarm");

	ret = write(fd, str, strlen(str) + 1);
	close(fd);

	if (ret < 0)
		err(1, "play tune");
	exit(0);
}

} // namespace

int
tone_alarm_main(int argc, char *argv[])
{
	unsigned tune;

	/* start the driver lazily */
	if (g_dev == nullptr) {
		g_dev = new ToneAlarm;

		if (g_dev == nullptr)
			errx(1, "couldn't allocate the ToneAlarm driver");

		if (g_dev->init() != OK) {
			delete g_dev;
			errx(1, "ToneAlarm init failed");
		}
	}

	if ((argc > 1) && !strcmp(argv[1], "start"))
		play_tune(1);

	if ((argc > 1) && !strcmp(argv[1], "stop"))
		play_tune(0);

	if ((tune = strtol(argv[1], nullptr, 10)) != 0)
		play_tune(tune);

	/* if it looks like a PLAY string... */
	if (strlen(argv[1]) > 2) {
		const char *str = argv[1];
		if (str[0] == 'M') {
			play_string(str);
		}
	}

	errx(1, "unrecognised command, try 'start', 'stop' or an alarm number");
}
