# This program will construct ksyms.s.  Ksyms.s contains a symbol table
# for all the kernel symbols included in the file ksyms.lst.  The following
# variables are defined in ksym.s:
#
#	int symbol_table_size;		/* number of symbols */
#	struct {
#		void *value;		/* value of symbol */
#		char *name;		/* name of symbol */
#	} symbol_table[];
#
#

trap "rm -f ksyms.tmp ksyms.lst ; exit 1" 1 2 

sed -e '/^#/d' -e '/^[	 ]*$/d' ksyms.lst | sort > ksyms.tmp

echo '	.data
	.globl	_symbol_table_size, _symbol_table

_symbol_table_size:'
echo "	.long" `wc -l < ksyms.tmp`
echo '
_symbol_table:'
awk 'BEGIN {stringloc = 0}
{print "	.long " $1; print "	.long strings+" stringloc; \
        stringloc += length($1) + 1;}' ksyms.tmp
echo '
strings:'
awk '{print "	.ascii \"" $1 "\\0\""}' ksyms.tmp
rm -f ksyms.tmp


#
# Alternativly, if the kernel is c++ compiled:
# By using gsub() we can forse all function names to appear as extern "C".
# This allows linkable drivers written in C or C++ - Jon
# awk '{gsub(/__F.*/, "") ; print "	.ascii \"" $0 "\\0\""}' ksyms.tmp
