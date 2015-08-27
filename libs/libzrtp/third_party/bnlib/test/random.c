/*
 * Copyright (c) 1993, 1994  Colin Plumb.  All rights reserved.
 * For licensing and other legal details, see the file legal.c.
 *
 * Cryptographic random number generation.
 */

#include "first.h"
#include <string.h>

#include "kb.h"		/* For kbGet() other stuff */
#include "md5.h"
#include "noise.h"
#include "random.h"
#include "randpool.h"
#include "userio.h"

#include "kludge.h"

/*
 * This code uses the randpool.c code to generate random numbers.
 * That can be augmented with other techniques, such as the
 * ANSI X9.17 generator, but the X9.17 generator uses a key-generating
 * key which needs to be obtained from somewhere, and the location is
 * not entirely clear.  The randpool.c functions are entirely
 * adequate; extra layers are for belt-and-suspenders security and
 * compliance to standards.
 *
 * For generating long-lived secret keys, we go one more step:
 * actually keep track of (an estimate of) the amount of entropy
 * which is in the random number pool, and wait for events until
 * the amount of entropy accumulated is enough to make all of the
 * bits of the secret key truly random.  Of course, the guarantees
 * of cryptographic strength still apply even if this estimation
 * is faulty.
 */


/* Get some random bytes */
void
randBytes(byte *buf, unsigned len)
{
	randPoolGetBytes(buf, len);
}

/*
 * A handy utility for generating uniformly distributed random numbers
 * in a small range.
 */
unsigned
randRange(unsigned range)
{
	unsigned div, r;
	byte b[2];

	if (range <= 1)
		return 0;

	if (range <= 256) {
		div = 256/range;
		do {
			randBytes(b, 1);
			r = b[0]/div;
		} while (r >= range);
	} else {
		div = (unsigned)(65536/range);
		do {
			randBytes(b, 2);
			r = ((unsigned)b[0] << 8 | b[1])/div;
		} while (r >= range);
	}
	b[0] = b[1] = 0;
	return r;
}

#ifdef UNIX	/* Or we have popen() */
/*
 * Execute the command "string", adding the entropy from the data thus
 * gethered to the random number pool.  Because the pool is rather
 * slow and we want to encourage the use of lots of data, rather than
 * adding the data directly, the MD5 is taken and that is added to the
 * pool.
 */
int
randSourceSet(char const *string, unsigned len, int pri)
{
	FILE *f;
	struct MD5Context md5;
	char buf[256];
	int i;

	(void)len;	/* string is null-terminated */
	(void)pri;	/* Use every argument, regardless of priority */

	f = popen(string, "r");
	if (!f)
		return -1;
	MD5Init(&md5);
	while ((i = fread(buf, 1, sizeof(buf), f)) > 0)
		MD5Update(&md5, (unsigned char *)buf, i);
	pclose(f);
	MD5Final((unsigned char *)buf, &md5);
	randPoolAddBytes((unsigned char *)buf, 16);
	memset(buf, 0, sizeof(buf));
	return 0;
}
#endif

/*
 * True random bit handling
 */

/*
 * Truly random bits are difficult to get and must be carefully hoarded.
 * These functions use the randpool.c code to store the entropy, and provide
 * some bookkeeping on the count of bits of true (Shannon) entropy available
 * in the pool.
 *
 * For generating ordinary session keys, "as much entropy as you've got"
 * is good enough, and no accounting is done, except to get some entropy
 * to generate the random number seed file if necessary.
 *
 * But for generating long-lived secret key components, extraordinary
 * measures are called for.  In addition to what may have been available
 * from the random seed file, random data from timed keystrokes is
 * accumulated until enough is available.
 *
 * An estimate of the number of bits of true (Shannon) entropy in the pool
 * is kept in trueRandBits.  This is incremented when timed keystrokes
 * are available, and decremented when bits are explicitly consumed for
 * some purpose or another.  This counter is maintained here, scaled by
 * FRACBITS to count fractional bits for thoroughness.  (Thus, the name
 * "trueRandBits" is a bit misleading, since it actually counts sixteenths
 * of a bit, but I can't think of a better one.)
 *
 * randFlush is the pool-stirring function.  It is also called to
 * obliterate traces of old random bits after prime generation is
 * completed.  (Primes are the most carefully-guarded values in PGP.)
 */

#define FRACBITS 4
#define DERATING 0x28	/* 2.5 bits subtracted for derating */
static word32 trueRandBits = 0;	/* Bits of entropy in pool */

/*
 * Ensure that the random numbers generated by prior calls to randBytes
 * will never be recoverable from the contents of memory.  This doesn't
 * wipe memory to a fixed value (the entropy might come in handy for future
 * operations), it just runs the generators forward enough that the previous
 * state is irretrievable.
 *
 * This is called after prime generation, before the random data is saved
 * out, so it is protecting prime data and is particularly paranoid.
 */
void
randFlush(void)
{
	byte buf[16];
	int i;

	for (i = 0; i < 3; i++)	/* Zipper + Belt + Suspenders */
		randPoolStir();		/* Clean pseudo-random generator */
	memset(buf, 0, sizeof(buf));
	trueRandBits = 0;
}

/*
 * Given an event (typically a keystroke) coded by "event" at a random time,
 * add all randomness to the random pool, compute a (conservative) estimate
 * of the amount, add it to the pool, and return the amount of randomness.
 * (The return value is just for informational purposes.)
 *
 * Double events are okay, but three in a row is considered
 * suspicious and the randomness is counted as 0.
 *
 * As an extra precaution against key repeat or other very regular input
 * data, the entropy extimate is derived not from the time interval measured,
 * but from the minimum of it and the (absolute) difference between it and
 * the previous time interval, i.e. the second-order delta.
 */
unsigned
randEvent(int event)
{
	static int event1 = 0, event2 = 0;	/* Previous events */
	static word32 prevdelta;		/* Previous delta */
	word32 delta;		/* Time between last two events */
	unsigned cbits;		/* Entropy estimate, in bits. */
	word32 t;		/* Temprary value */
	int i;

	delta = noise();
	randPoolAddBytes((byte *)&event, sizeof(event));

	/*
	 * Don't credit triple events with any entropy on the grounds that
	 * they're probably something periodic like key repeat.  But remember
	 * the delta.
	 */
	if (event == event1 && event == event2) {
		prevdelta = delta;
		return 0;
	}

	event2 = event1;
	event1 = event;

	/* Compute second-order delta */
	t = (delta > prevdelta) ? delta - prevdelta : prevdelta - delta;
	/* Remember current delta for next time */
	prevdelta = delta;
	/* Find minimum of delta and second-order delta */
	if (delta > t)
		delta = t;

	/* Avoid divide-by-zero errors below */
	if (!delta)
		return 0;

	/* Count the number of bits of entropy available - integer log2. */
	cbits = 0;
	i = 16;
	t = 0xffffffff;
	do {
		t <<= i;
		if (delta & t)
			cbits += i;
		else
			delta <<= i;
	} while (i >>= 1);

	/*
	 * At this point, delta is normalized and has its high bit set.
	 * Now count fractional bits, using binary logarithm algorithm
	 */
	for (i = 0; i < FRACBITS; i++) {
		cbits <<= 1;
		delta >>= 16;
		delta *= delta;
		if (delta & 0x80000000)
			cbits++;
		else
			delta <<= 1;
	}

	if (cbits <= DERATING)
		return 0;	/* nothing */
	cbits -= DERATING;
	trueRandBits += cbits;
	if (trueRandBits > RANDPOOLBITS<<FRACBITS)
		trueRandBits = RANDPOOLBITS<<FRACBITS;

	return cbits;
}

/*
 * Performs an accumulation of random bits.  As long as there are
 * fewer bits in the buffer than are needed, prompt for more.
 * (kbGet is known to call randEvent() which increments trueRandBits.)
 */
void
randAccum(unsigned count)
{
	word32 randbits = trueRandBits;

	noise();	/* Establish a baseline for timing comparisons */

	if (count > RANDPOOLBITS)
		count = RANDPOOLBITS;

	if (randbits>>FRACBITS >= count)
		return;

	userPrintf("\n\
We need to generate %u random bits.  This is done by measuring the\n\
time intervals between your keystrokes.  Please enter some random text\n\
on your keyboard until you hear the beep:\n", count - (randbits>>FRACBITS));

	kbCbreak();

	do {
		/* display counter to show progress */
		userPrintf(("\r%4u "), count-(unsigned)(randbits>>FRACBITS));
		userFlush();	/* ensure screen update */

		kbFlush(0);	/* Typeahead is illegal */
		(void)kbGet();	/* Wait for next char */

		/* Print flag indicating acceptance (or not) */
		userPutc(trueRandBits == randbits ? '?' : '.');
		randbits = trueRandBits;
	} while (randbits>>FRACBITS < count);

	/* Do final display update */
	userPuts(("\r   0 *"));
	userPuts("\a -Enough, thank you.\n");

	/* Do an extra-thorough flush to absorb extra typing. */
	kbFlush(1);

	kbNorm();
}
