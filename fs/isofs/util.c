/*
 *  linux/fs/isofs/util.c
 *
 *  The special functions in the file are numbered according to the section
 *  of the iso 9660 standard in which they are described.  isonum_733 will
 *  convert numbers according to section 7.3.3, etc.
 *
 *  isofs special functions.  This file was lifted in its entirety from
 * the bsd386 iso9660 filesystem, by Pace Williamson.
 */


int
isonum_711 (char * p)
{
	return (*p & 0xff);
}

int
isonum_712 (char * p)
{
	int val;
	
	val = *p;
	if (val & 0x80)
		val |= 0xffffff00;
	return (val);
}

int
isonum_721 (char * p)
{
	return ((p[0] & 0xff) | ((p[1] & 0xff) << 8));
}

int
isonum_722 (char * p)
{
	return (((p[0] & 0xff) << 8) | (p[1] & 0xff));
}

int
isonum_723 (char * p)
{
#if 0
	if (p[0] != p[3] || p[1] != p[2]) {
		fprintf (stderr, "invalid format 7.2.3 number\n");
		exit (1);
	}
#endif
	return (isonum_721 (p));
}

int
isonum_731 (char * p)
{
	return ((p[0] & 0xff)
		| ((p[1] & 0xff) << 8)
		| ((p[2] & 0xff) << 16)
		| ((p[3] & 0xff) << 24));
}

int
isonum_732 (char * p)
{
	return (((p[0] & 0xff) << 24)
		| ((p[1] & 0xff) << 16)
		| ((p[2] & 0xff) << 8)
		| (p[3] & 0xff));
}

int
isonum_733 (char * p)
{
#if 0
	int i;

	for (i = 0; i < 4; i++) {
		if (p[i] != p[7-i]) {
			fprintf (stderr, "bad format 7.3.3 number\n");
			exit (1);
		}
	}
#endif
	return (isonum_731 (p));
}

/* We have to convert from a MM/DD/YY format to the unix ctime format.  We have to
   take into account leap years and all of that good stuff.  Unfortunately, the kernel
   does not have the information on hand to take into account daylight savings time,
   so there will be cases (roughly half the time) where the dates are off by one hour. */
int iso_date(char * p, int flag)
{
	int year, month, day, hour ,minute, second, tz;
	int crtime, days, i;

	year = p[0] - 70;
	month = p[1];
	day = p[2];
	hour = p[3];
	minute = p[4];
	second = p[5];
	if (flag == 0) tz = p[6]; /* High sierra has no time zone */
	else tz = 0;
	
	if (year < 0) {
		crtime = 0;
	} else {
		int monlen[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
		days = year * 365;
		if (year > 2)
			days += (year+1) / 4;
		for (i = 1; i < month; i++)
			days += monlen[i-1];
		if (((year+2) % 4) == 0 && month > 2)
			days++;
		days += day - 1;
		crtime = ((((days * 24) + hour) * 60 + minute) * 60)
			+ second;
		
		/* sign extend */
		if (tz & 0x80)
			tz |= (-1 << 8);
		
		/* timezone offset is unreliable on some disks */
		if (-48 <= tz && tz <= 52)
			crtime += tz * 15 * 60;
	}
	return crtime;
}		
	
