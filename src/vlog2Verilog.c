//----------------------------------------------------------------
// vlog2Verilog
//----------------------------------------------------------------
// Convert between verilog styles.
// Options include bit-blasting vectors and adding power
// supply connections.
//
// Revision 0, 2018-11-29: First release by R. Timothy Edwards.
//
// This program is written in ISO C99.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>	/* For getopt() */
#include <math.h>
#include <ctype.h>
#include <float.h>

#include "hash.h"
#include "readverilog.h"
#include "readlef.h"

int write_output(struct cellrec *, unsigned char, char *);
void helpmessage(FILE *outf);
void cleanup_string(char *);

char *VddNet = NULL;
char *GndNet = NULL;

struct hashtable Lefhash;

/* Define option flags */

#define	IMPLICIT_POWER	(unsigned char)0x01
#define	MAINTAIN_CASE	(unsigned char)0x02
#define	BIT_BLAST	(unsigned char)0x04
#define	NONAME_POWER	(unsigned char)0x08

/*--------------------------------------------------------------*/

int main (int argc, char *argv[])
{
    int i, result;
    unsigned char Flags;

    char *vloginname = NULL;
    char *vlogoutname = NULL;
    struct cellrec *topcell;

    Flags = (unsigned char)IMPLICIT_POWER;

    VddNet = strdup("VDD");
    GndNet = strdup("VSS");

    InitializeHashTable(&Lefhash, SMALLHASHSIZE);

    while ((i = getopt(argc, argv, "pbchnHv:g:l:o:")) != EOF) {
	switch( i ) {
	    case 'p':
		Flags &= ~IMPLICIT_POWER;
		break;
	    case 'b':
		Flags |= BIT_BLAST;
		break;
	    case 'c':
		Flags |= MAINTAIN_CASE;
		break;
	    case 'n':
		Flags |= NONAME_POWER;
		break;
	    case 'h':
	    case 'H':
		helpmessage(stdout);
		exit(0);
		break;
	    case 'l':
		LefRead(optarg);		/* Can be called multiple times */
		break;
	    case 'v':
		free(VddNet);
		VddNet = strdup(optarg);
		cleanup_string(VddNet);
		break;
	    case 'o':
		vlogoutname = strdup(optarg);
		break;
	    case 'g':
		free(GndNet);
		GndNet = strdup(optarg);
		cleanup_string(GndNet);
		break;
	    default:
		fprintf(stderr,"Bad switch \"%c\"\n", (char)i);
		helpmessage(stderr);
		return 1;
	}
    }

    if (optind < argc) {
	vloginname = strdup(argv[optind]);
	optind++;
    }
    else {
	fprintf(stderr, "Couldn't find a filename as input\n");
	helpmessage(stderr);
	return 1;
    }
    optind++;

    /* If any LEF files were read, hash the GateInfo list */
    if (GateInfo != NULL) {
	GATE gate;
	for (gate = GateInfo; gate; gate = gate->next) {
	    HashPtrInstall(gate->gatename, gate, &Lefhash);
	}
    }

    topcell = ReadVerilog(vloginname);
    result = write_output(topcell, Flags, vlogoutname);
    return result;
}

/*--------------------------------------------------------------*/
/* String input cleanup	(mainly strip quoted text)		*/
/*--------------------------------------------------------------*/

void cleanup_string(char *text)
{
    int i;
    char *sptr, *wptr;

    /* Remove quotes from quoted strings */

    sptr = strchr(text, '"');
    if (sptr != NULL) {
       i = 0;
       while (sptr[i + 1] != '"') {
          sptr[i] = sptr[i + 1];
          i++;
       }
       sptr[i] = '\0';
    }
}

/*--------------------------------------------------------------------------*/
/* Recursion callback function for each item in the cellrec nets hash table */
/*--------------------------------------------------------------------------*/

struct nlist *output_wires(struct hashlist *p, void *cptr)
{
    struct netrec *net;
    FILE *outf = (FILE *)cptr;

    net = (struct netrec *)(p->ptr);

    /* Ignore the power and ground nets;  these have already been output */
    if (p->name[0] == '1' && p->name[1] == '\'') {
	char c = p->name[2];
	char b = p->name[3];
	if (c == 'b' || c == 'h' || c == 'd' || c == 'o')
	    return NULL;
    }

    fprintf(outf, "wire ");
    if (net->start >= 0 && net->end >= 0) {
	fprintf(outf, "[%d:%d] ", net->start, net->end);
    }
    fprintf(outf, "%s ;\n", p->name);
    return NULL;
}

/*----------------------------------------------------------------------*/
/* Recursion callback function for each item in the cellrec properties  */
/* hash table                                                           */
/*----------------------------------------------------------------------*/

struct nlist *output_props(struct hashlist *p, void *cptr)
{
    char *propval = (char *)(p->ptr);
    FILE *outf = (FILE *)cptr;

    fprintf(outf, ".%s(%s),\n", p->name, propval);
    return NULL;
}

/*--------------------------------------------------------------*/
/* write_output							*/
/*								*/
/*         ARGS: 						*/
/*      RETURNS: 1 to OS					*/
/* SIDE EFFECTS: 						*/
/*--------------------------------------------------------------*/

int write_output(struct cellrec *topcell, unsigned char Flags, char *outname)
{
    FILE *outfptr = stdout;
    int result = 0;

    struct netrec *net;
    struct portrec *port;
    struct instance *inst;

    if (outname != NULL) {
	outfptr = fopen(outname, "w");
	if (outfptr == NULL) {
	    fprintf(stderr, "Error:  Cannot open file %s for writing.\n", outname);
	    return 1;
	}
    }

    /* Write output module header */
    fprintf(outfptr, "/* Verilog module written by vlog2Verilog (qflow) */\n");

    if (Flags & IMPLICIT_POWER)
        fprintf(outfptr, "/* With explicit power connections */\n");
    if (!(Flags & MAINTAIN_CASE))
        fprintf(outfptr, "/* With case-insensitive names (SPICE-compatible) */\n");
    if (Flags & BIT_BLAST)
        fprintf(outfptr, "/* With bit-blasted vectors */\n");
    if (Flags & NONAME_POWER)
        fprintf(outfptr, "/* With power connections converted to binary 1, 0 */\n");
    fprintf(outfptr, "\n");

    fprintf(outfptr, "module %s(\n", topcell->name);

    if (Flags & IMPLICIT_POWER) {
	fprintf(outfptr, "    inout %s,\n", VddNet);
	fprintf(outfptr, "    inout %s,\n", GndNet);
    }

    for (port = topcell->portlist; port; port = port->next) {
	if (port->name == NULL) continue;
	switch(port->direction) {
	    case PORT_INPUT:
		fprintf(outfptr, "    input ");
		break;
	    case PORT_OUTPUT:
		fprintf(outfptr, "    output ");
		break;
	    case PORT_INOUT:
		fprintf(outfptr, "    inout ");
		break;
	}
	net = HashLookup(port->name, &topcell->nets);
	if (net && net->start >= 0 && net->end >= 0) {
	    fprintf(outfptr, "[%d:%d] ", net->start, net->end);
	}
	fprintf(outfptr, "%s", port->name);
	if (port->next) fprintf(outfptr, ",");
	fprintf(outfptr, "\n");
    }
    fprintf(outfptr, ");\n\n");

    /* Declare all wires */

    if (!(Flags & IMPLICIT_POWER) && !(Flags & NONAME_POWER)) {
	fprintf(outfptr, "wire %s = 1'b1;\n", VddNet);
	fprintf(outfptr, "wire %s = 1'b0;\n\n", GndNet);
    }

    RecurseHashTablePointer(&topcell->nets, output_wires, outfptr);
    fprintf(outfptr, "\n");

    /* Write instances in the order of the input file */

    for (inst = topcell->instlist; inst; inst = inst->next) {
	int nprops = RecurseHashTable(&inst->propdict, CountHashTableEntries);
	fprintf(outfptr, "%s ", inst->cellname);
	if (nprops > 0) {
	    fprintf(outfptr, "#(\n");
	    RecurseHashTablePointer(&inst->propdict, output_props, outfptr);
	    fprintf(outfptr, ") ");
	}
	if (inst->cellname)
	    fprintf(outfptr, "%s (\n", inst->instname);
	else {
	    fprintf(outfptr, "vlog2Verilog:  No cell for instance %s\n", inst->instname);
	    result = 1;		// Set error result but continue output.
	}

	if (Flags & IMPLICIT_POWER) {

	    /* If any LEF files were read, then get the power and	*/
	    /* ground net names from the LEF file definition.		*/

	    if (GateInfo != NULL) {
		GATE gate = (GATE)HashLookup(inst->cellname, &Lefhash);
		if (gate) {
		    int n;
		    u_char found = 0;
		    for (n = 0; n < gate->nodes; n++) {
			if (gate->use[n] == PORT_USE_POWER) {
			    fprintf(outfptr, "    .%s(%s),\n", gate->node[n], VddNet);
			    found++;
			}
			else if (gate->use[n] == PORT_USE_GROUND) {
			    fprintf(outfptr, "    .%s(%s),\n", gate->node[n], GndNet);
			    found++;
			}
			if (found == 2) break;
		    }
		}
		else {
		    /* Fall back on VddNet and GndNet names */
		    fprintf(outfptr, "    .%s(%s),\n", GndNet, GndNet);
		    fprintf(outfptr, "    .%s(%s),\n", VddNet, VddNet);
		}
	    }
	    else {
		fprintf(outfptr, "    .%s(%s),\n", GndNet, GndNet);
		fprintf(outfptr, "    .%s(%s),\n", VddNet, VddNet);
	    }
	}

	/* Write each port and net connection */
	for (port = inst->portlist; port; port = port->next) {

	    /* If writing explicit power net names, then watch	*/
	    /* for power connections encoded as binary, and	*/
	    /* convert them to the power bus names.		*/

	    if ((Flags & IMPLICIT_POWER) || (!(Flags & NONAME_POWER))) {
		if (port->net[0] == '1' && port->net[1] == '\'') {
		    char c = port->net[2];
		    char b = port->net[3];
		    if (c == 'b' || c == 'h' || c == 'd' || c == 'o') {
			if (b == '1') {
			    free(port->net);
			    port->net = strdup(VddNet);
			 }
			else if (b == '0') {
			    free(port->net);
			    port->net = strdup(GndNet);
			}
		    }
		}
	    }
	    fprintf(outfptr, "    .%s(%s)", port->name, port->net);
	    if (port->next) fprintf(outfptr, ",");
	    fprintf(outfptr, "\n");
	}
	fprintf(outfptr, ");\n\n");
    }

    /* End the module */
    fprintf(outfptr, "endmodule\n");

    if (outname != NULL) fclose(outfptr);

    fflush(stdout);
    return result;
}

/*--------------------------------------------------------------*/
/* C helpmessage - tell user how to use the program		*/
/*								*/
/*         ARGS: 						*/
/*      RETURNS: 1 to OS					*/
/* SIDE EFFECTS: 						*/
/*--------------------------------------------------------------*/

void helpmessage(FILE *outf)
{
    fprintf(outf, "vlog2Verilog [-options] <netlist>\n");
    fprintf(outf, "\n");
    fprintf(outf, "vlog2Verilog converts a netlist in one verilog style to\n");
    fprintf(outf, "another. LEF files may be given as inputs to determine\n");
    fprintf(outf, "power and ground net names for cells.\n");
    fprintf(outf, "\n");
    fprintf(outf, "options:\n");
    fprintf(outf, "\n");
    fprintf(outf, "  -h         Print this message\n");    
    fprintf(outf, "  -o <path>  Set output filename (otherwise output is on stdout).\n");    
    fprintf(outf, "  -p         Don't add power nodes to instances\n");
    fprintf(outf, "             (only nodes present in the instance used)\n");
    fprintf(outf, "  -b         Remove vectors (bit-blasted)\n");
    fprintf(outf, "  -c         Case-insensitive output (SPICE compatible) \n");
    fprintf(outf, "  -n         Convert power nets to binary 1 and 0\n");
    fprintf(outf, "  -l <path>  Read LEF file from <path>\n");
    fprintf(outf, "  -v <name>  Use <name> for power net (default \"Vdd\")\n");
    fprintf(outf, "  -g <name>  Use <name> for ground net (default \"Gnd\")\n");

} /* helpmessage() */
