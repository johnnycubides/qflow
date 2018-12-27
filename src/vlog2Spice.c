//--------------------------------------------------------------
// vlog2Spice
//
// Convert a structural verilog netlist (with power and ground
// nets!) into a SPICE netlist.
//
// Revision 0, 2006-11-11: First release by R. Timothy Edwards.
// Revision 1, 2009-07-13: Minor cleanups by Philipp Klaus Krause.
// Revision 2, 2013-05-10: Modified to take a library of subcell
//		definitions to use for determining port order.
// Revision 3, 2013-10-09: Changed from BDnet2BSpice to
//		blif2BSpice
// Revision 4, 2018-11-28: Changed from blif2BSpice to vlog2Spice
//
//--------------------------------------------------------------

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <ctype.h>
#include <float.h>

#include "hash.h"
#include "readverilog.h"

#define LengthOfLine    	16384

/* Function prototypes */
int write_output(struct cellrec *, char *, char *, int);
int loc_getline(char s[], int lim, FILE *fp);
void helpmessage(FILE *);

#define DO_INCLUDE   0x01
#define DO_DELIMITER 0x02

//--------------------------------------------------------

int main (int argc, char *argv[])
{
    int i, result = 0;
    int flags = 0;

    char *vloginname = NULL;
    char *spclibname = NULL;
    char *spcoutname = NULL;

    struct cellrec *topcell = NULL;

    while ((i = getopt(argc, argv, "hHidl:s:o:")) != EOF) {
	switch (i) {
	    case 'l':
		spclibname = strdup(optarg);
		break;
	    case 'o':
		spcoutname = strdup(optarg);
		break;
	    case 'i':
		flags |= DO_INCLUDE;
		break;
	    case 'd':
		flags |= DO_DELIMITER;
		break;
	    case 'h':
	    case 'H':
		helpmessage(stdout);
		exit(0);
		break;
	    default:
		fprintf(stderr, "Unknown switch %c\n", (char)i);
		helpmessage(stderr);
		exit(1);
		break;
	}
    }

    if (optind < argc) {
	vloginname = strdup(argv[optind]);
	optind++;
    }
    else {
	fprintf(stderr, "Couldn't find a filename as input\n");
	helpmessage(stderr);
	exit(1);
    }
    optind++;

    topcell = ReadVerilog(vloginname);
    result = write_output(topcell, spclibname, spcoutname, flags);

    return result;
}

/*--------------------------------------------------------------*/
/* write_output ---  Write the SPICE netlist output		*/
/*								*/
/* ARGS: 							*/
/* RETURNS: 0 on success, 1 on error.				*/
/* SIDE EFFECTS: 						*/
/*--------------------------------------------------------------*/

int write_output(struct cellrec *topcell, char *libname, char *outname, int flags)
{
    FILE *libfile;
    FILE *outfile;

    struct netrec *net;
    struct instance *inst;
    struct portrec *port;
    struct portrec *newport, *portlist, *lastport;

    int i, j, k, start, end, result = 0;

    char *lptr;
    char *sp, *sp2;
    char line[LengthOfLine];

    struct hashtable Libhash;

    // Read a SPICE library of subcircuits and use it to define the order
    // of pins that were read from LEF (which is not necessarily in SPICE
    // pin order).

    if (libname) {
	libfile = fopen(libname, "r");
	if (libfile == NULL) {
	    fprintf(stderr, "Couldn't open %s for reading\n", libname);
	    return 1;
	}
    }

    if (outname != NULL) {
	outfile = fopen(outname, "w");
	if (outfile == NULL) {
	    fprintf(stderr, "Error:  Couldn't open file %s for writing\n", outname);
	    return 1;
	}
    }
    else
	outfile = stdout;

    /* Initialize SPICE library hash table */
    InitializeHashTable(&Libhash, SMALLHASHSIZE);

    /* Read SPICE library of subcircuits, if one is specified.	*/
    /* Retain the name and order of ports passed to each	*/
    /* subcircuit.						*/

    j = 0;
    while (loc_getline(line, sizeof(line), libfile) > 0) {
	if (!strncasecmp(line, ".subckt", 7)) {
	    char *cellname;
	    lastport = NULL;
	    portlist = NULL;

	    /* Read cellname */
	    sp = line + 7;
	    while (isspace(*sp) && (*sp != '\n')) sp++;
	    sp2 = sp;
	    while (!isspace(*sp2) && (*sp2 != '\n')) sp2++;
	    *sp2 = '\0';

	    /* Keep a record of the cellname until we generate the hash entry for it */
	    cellname = strdup(sp);

	    /* Now fill out the ordered port list */
	    sp = sp2 + 1;
	    while (isspace(*sp) && (*sp != '\n') && (*sp != '\0')) sp++;
	    while (sp) {

		/* Move string pointer to next port name */

		if (*sp == '\n' || *sp == '\0') {
		    loc_getline(line, sizeof(line), libfile);
		    if (*line == '+') {
			sp = line + 1;
			while (isspace(*sp) && (*sp != '\n')) sp++;
		    }
		    else
			break;
		}

		/* Terminate port name and advance pointer */
		sp2 = sp;
		while (!isspace(*sp2) && (*sp2 != '\n') && (*sp2 != '\0')) sp2++;
		*sp2 = '\0';

		/* Add port to list (in forward order) */

		newport = (struct portrec *)malloc(sizeof(struct portrec));
		if (portlist == NULL)
		    portlist = newport;
		else
		    lastport->next = newport;
		lastport = newport;
		newport->name = strdup(sp);
		newport->net = NULL;
		newport->direction = 0;
		newport->next = NULL;

		sp = sp2 + 1;
	    }

	    /* Read input to end of subcircuit */

	    if (strncasecmp(line, ".ends", 4)) {
		while (loc_getline(line, sizeof(line), libfile) > 0)
		    if (!strncasecmp(line, ".ends", 4))
			break;
	    }

	    /* Hash the new port record by cellname */
	    HashPtrInstall(cellname, portlist, &Libhash);
	    free(cellname);
	}
    }

    /* Write output header */
    fprintf(outfile, "*SPICE netlist created from verilog structural netlist module "
			"%s by vlog2Spice (qflow)\n", topcell->name);
    fprintf(outfile, "*This file may contain array delimiters, not for use in simulation.\n");
    fprintf(outfile, "\n");

    /* If flags has DO_INCLUDE then dump the contents of the	*/
    /* libraray.  If 0, then just write a .include line.	*/

    if (flags & DO_INCLUDE) {
	if (libname != NULL) {

	    /* Write out the subcircuit library file verbatim */
	    rewind(libfile);
	    while (loc_getline(line, sizeof(line), libfile) > 0)
		fputs(line, outfile);
	    fclose(libfile);
	}
    }
    else {
	fprintf(outfile, ".include %s\n", libname);
    }
    fprintf(outfile, "\n");

    /* Generate the subcircuit definition, adding power and ground nets */

    fprintf(outfile, ".subckt %s ", topcell->name);

    for (port = topcell->portlist; port; port = port->next) {
	if ((net = BusHashLookup(port->name, &topcell->nets)) != NULL) {
	    start = net->start;
	    end = net->end;
	}
	else start = end = -1;

	if (start > end) {
	    int tmp;
	    tmp = start;
	    start = end;
	    end = tmp;
	}
	if (start == end) {
	    fprintf(outfile, "%s ", port->name);
	}
	else {
	    for (i = start; i <= end; i++) {
		/* Note that use of brackets is not legal SPICE syntax	*/
		/* but suffices for LVS and such.  Output should be	*/
		/* post-processed before using in simulation.		*/
		if (flags & DO_DELIMITER)
		    fprintf(outfile, "%s<%d> ", port->name, i);
		else
		    fprintf(outfile, "%s[%d] ", port->name, i);
	    }
	}
    }
    fprintf(outfile, "\n\n");

    /* Output instances */

    for (inst = topcell->instlist; inst; inst = inst->next) {

	struct portrec *libport;

	/* Search library records for subcircuit */

	portlist = (struct portrec *)HashLookup(inst->cellname, &Libhash);
	if (portlist == NULL) {
	    fprintf(stderr, "Error:  No such gate \"%s\" in SPICE library!\n",
			inst->cellname);
	    result = 1;		// Mark as error but continue output.
	    continue;
	}
	fprintf(outfile, "X%s ", inst->instname);

	/* Output pin connections in the order of the LEF record, which	*/
	/* has been forced to match the port order of the SPICE library	*/

	for (libport = portlist; libport; libport = libport->next) {
	    for (port = inst->portlist; port; port = port->next) {
		if (!strcasecmp(libport->name, port->name)) {
		    if (flags & DO_DELIMITER) {
			char *d1ptr, *d2ptr;
			if ((d1ptr = strchr(port->net, '[')) != NULL) {
			    if ((d2ptr = strchr(d1ptr + 1, ']')) != NULL) {
				*d1ptr = '<';
				*d2ptr = '>';
			    }
			}
		    }
		    fprintf(outfile, "%s ", port->net);
		    break;
		}
	    }
	}
	fprintf(outfile, "%s\n", inst->cellname);
    }
    fprintf(outfile, "\n.ends\n");
    fprintf(outfile, ".end\n");

    if (outname != NULL) fclose(outfile);

    return result;
}

/*--------------------------------------------------------------*/
/*C loc_getline: read a line, return length			*/
/*								*/
/* ARGS: 							*/
/* RETURNS: 1 to OS						*/
/* SIDE EFFECTS: 						*/
/*--------------------------------------------------------------*/

int loc_getline(char *s, int lim, FILE *fp)
{
    int c, i;
	
    i = 0;
    while(--lim > 0 && (c = getc(fp)) != EOF && c != '\n')
	s[i++] = c;
    if (c == '\n')
	s[i++] = c;
    s[i] = '\0';
    if (c == EOF) i = 0; 
    return i;
}

/*--------------------------------------------------------------*/
/*C helpmessage - tell user how to use the program		*/
/*								*/
/*  ARGS: error code (0 = success, 1 = error)			*/
/*  RETURNS: 1 to OS						*/
/*  SIDE EFFECTS: 						*/
/*--------------------------------------------------------------*/

void helpmessage(FILE *fout)
{
    fprintf(fout, "vlog2Spice [-options] netlist \n");
    fprintf(fout, "\n");
    fprintf(fout, "vlog2Spice converts a netlist in verilog format \n");
    fprintf(fout, "to Spice subcircuit format. Output on stdout unless -o option used.\n");
    fprintf(fout, "Input file must be a structural verilog netlist with power and ground.\n");
    fprintf(fout, "\n");
    fprintf(fout, "Options:\n");
    fprintf(fout, "   -h          Print this message\n");    
    fprintf(fout, "   -i          Generate include statement for library, not a dump.\n");
    fprintf(fout, "   -d          Convert array delimiter brackets to angle brackets.\n");
    fprintf(fout, "   -l <path>   Specify path to SPICE library of standard cells.\n");
    fprintf(fout, "   -o <path>   Specify path to output SPICE file.\n");
    fprintf(fout, "\n");

} /* helpmessage() */
