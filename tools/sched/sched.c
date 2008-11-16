/*
 * Copyright 2007, 2008 by Brian Dominy <brian@oddchange.com>
 *
 * This file is part of FreeWPC.
 *
 * FreeWPC is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * FreeWPC is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with FreeWPC; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */


/*
 * This program implements a static scheduler based on a periodic interrupt.
 * FreeWPC uses this to generate an interrupt handler that calls the appropriate
 * subroutines that need realtime scheduling.
 *
 * Syntax: sched [options] [input-files]
 * Options:
 * -o <file>       Write the generated C code to <file>.  If not specified,
 *                 stdout is used.
 *
 * -i <include>    Add a #include to the generated code.  This can be
 *                 given multiple times.  The #include lines are just
 *                 inserted at the top of the output.
 *
 * -M <max-ticks>  The maximum amount of unrolling to occur, in ticks. (8)
 *
 * -p <prefix>     The prefix to use on all autogenerated code declarations.
 *                 This could be used if multiple schedules need to be
 *                 compiled into a single program.
 *
 * Each input file is a list of items to be scheduled, generally as follows:
 * <name> <period> <length>
 *
 * where 'name' is the C function name to be invoked, 'period' is how often
 * this task should be called, and 'length' is how long on average it takes
 * this call to run to completion.
 *
 * The period and length are given as the number of periodic interrupts.
 * This is system-dependent; on WPC, 1 interrupt = 976 microseconds.
 * period must be a power of 2.  length can be any value, including a
 * fractional one.  length can also be given in CPU cycles, by appending
 * a 'c' suffix to the value.
 *
 * The scheduler performs a 'load balancing' function based on the duration
 * of each task.  It tries to place tasks into equal-sized buckets, so that
 * on each interrupt, roughly the same amount of CPU is used.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_ID 128
#define MAX_TICKS 32
#define MAX_SLOTS_PER_TICK 32
#define MAX_TASKS 64
#define MAX_INCLUDE_FILES 16
#define MAX_CONDITIONALS 32

/* The following defines are system-dependent, and could be
changed to support non-FreeWPC compilations. */

#define ATTR_INTERRUPT "__interrupt__"

#define ATTR_FASTVAR "__attribute__((section (\"direct\")))"

#define CYCLES_PER_TICK 1952

#define CYCLES_PER_CALL 7

#define CYCLES_PER_RETURN 5


struct include_file
{
	char name[MAX_ID];
};


struct task
{
	/* The function to be called to run this task.  The
	function must take no parameters and not return anything. */
	char name[MAX_ID];

	/* The frequency in ticks that the task should be called */
	unsigned int period;

	/* The estimated length of time, in ticks, that it takes
	this task to complete during each iteration */
	double len;

#ifdef FUTURE
	/* Nonzero if the function uses the "next" macro to finish
	rather than just returning.  This allows the function to
	call the next function in the chain directly, without
	the need for a call and return.  This is optional. */
	unsigned int next_p;
#endif

	int already_unrolled_count;

	/* The number of slots in which this task is scheduled */
	int n_slots;
};


/* A slot = invocation of a task */

struct slot
{
	unsigned int divider;
	struct task *task;
};

/* A tick = interrupt handler */

struct tick
{
	unsigned int n_slots;
	struct slot slots[MAX_SLOTS_PER_TICK];
	double len;
};


/* The master scheduling table.  Each 'unrolled' version
of the handler is assigned to a different element in ticks.
n_ticks is the number of such ticks that are being used. */
unsigned int n_ticks = 0;
struct tick ticks[MAX_TICKS];

/* The number of tasks that have been declared.  A task may
be scheduled to one or more ticks.  The task object stores
the common information. */
unsigned int n_tasks = 0;
struct task tasks[MAX_TASKS];

/* The configured maximum of number of ticks to use.  Unrolling 
will not happen more than this. */
unsigned int max_ticks = 8;

/* The maximum divider needed.  A divider is needed when a task
needs to run less frequently than the amount of unrolling; a
runtime variable must be maintained to keep count of the number
of calls, and to execute the code conditionally. */
unsigned int max_divider = 1;

/* The prefix to use on all autogenerated functions.  This allows
more than one instance of autogenerated code to live together.
Do not include an underscore here */
const char *prefix = "tick";

/* The list of include filenames that need to be written out. */
unsigned int n_includes = 0;
struct include_file include_files[MAX_INCLUDE_FILES];

double divider_overhead = 0.05;

double slot_overhead = 0.01;

const unsigned int cycles_per_interrupt = CYCLES_PER_TICK;

double warn_utilization_high = 0.80;

int n_conditionals = 0;
const char *conditionals[MAX_CONDITIONALS];

#if 0
static unsigned long gcd (unsigned long a, unsigned long b)
{
	if (b == 0)
		return a;
	else
		return gcd (b, a % b);
}


static unsigned long lcm (unsigned long a, unsigned long b)
{
	return (a  / gcd (a, b)) * b;
}
#endif


#define cfprintf(ind, file, format, rest...) \
do { \
	unsigned int _n; \
	for (_n = 0; _n < ind; _n++) \
      fputc ('\t', file); \
	fprintf (file, format, ## rest); \
} while (0)


#define c_block_begin(ind, file) \
	do { cfprintf (ind, file, "{\n"); ind++; } while (0)


#define c_block_end(ind, file) \
	do { ind--; cfprintf (ind, file, "}\n"); } while (0)


static void write_comment (unsigned int indent, FILE *f, const char *comment)
{
	cfprintf (indent, f, "/* ");
	cfprintf (indent, f, "%s */\n", comment);
}


static void write_time_comment (FILE *f, double time)
{
	fprintf (f, "/* %g interrupts / %g cycles */",
		time, time * cycles_per_interrupt);
}


/**
 * Write the driver code to the output file.
 */
void write_tick_driver (FILE *f)
{
	unsigned int n;
	unsigned int div;
	char task_name[MAX_ID];
	unsigned int indent = 0;
	double tick_len;

	/* Write preliminary definitions */

	write_comment (indent, f, "Automatically generated by gensched");

	fprintf (f, ATTR_FASTVAR " void (*%s_function) (void);\n\n", prefix);
	fprintf (f, ATTR_FASTVAR " unsigned char %s_divider;\n", prefix);

	for (n=0; n < n_includes; n++)
		fprintf (f, "#include \"%s\"\n", include_files[n].name);
	fprintf (f, "\n");

	/* Check for tasks that could be improved */

	for (n=0; n < n_tasks; n++)
	{
		struct task *task = &tasks[n];
		int inline_p = task->name[0] == '!';
		unsigned int cycles;

		/* Warn if a large inline function is called multiple times.  Such a
		 * function is better not inline, as it is currently eating a lot
		 * of code space. */
		if (inline_p && task->n_slots > 2 && task->len > 200)
		{
			fprintf (stderr, "warning: %s should not be inline\n", task->name+1);
		}

		/* Warn if any small function is not inline, when it should be.
		 * Such a function is consuming lots of cycles for the call/return.
		 */
		cycles = (int)(task->len * CYCLES_PER_TICK);
		if (!inline_p && cycles < 40)
		{
			fprintf (stderr, "warning: %s should be inline, only takes %d cycles\n",
				task->name, cycles);
		}
	}
	fprintf (f, "\n");

	/* Write prototypes for each interrupt function. */

	for (n=0; n < n_ticks; n++)
		fprintf (f, "static " ATTR_INTERRUPT " void %s_%d (void);\n", prefix, n);
	fprintf (f, "\n");

	/* Write definitions for each interrupt function. */

	for (n=0; n < n_ticks; n++)
	{
		struct tick *tick = &ticks[n];
		unsigned int slotno;
		unsigned int divider_count = 0;

		tick_len = tick->len;

		fprintf (f, "static " ATTR_INTERRUPT " void %s_%d (void)\n", prefix, n);
		c_block_begin (indent, f);

		for (div = 1; div <= max_divider; div *= 2)
		{
			unsigned int used = 0;
			unsigned int inline_p;

			for (slotno = 0; slotno < tick->n_slots; slotno++)
			{
				struct slot *slot = &tick->slots[slotno];
				if (slot->divider == div)
				{
					if ((div > 1) && (used == 0))
					{
						fprintf (f, "\n");
						cfprintf (indent, f, "if (!(%s_divider & %d))\n",
							prefix, div-1);
						c_block_begin (indent, f);
						used = 1;
						divider_count++;
					}

					/* A leading '!' character on the task name means that
					the function is implemented as an inline macro.
					This is optional, but knowing this allows for some
					optimization suggestions. */
					inline_p = (*slot->task->name == '!');
					strcpy (task_name, slot->task->name + inline_p);

					if (slot->task->already_unrolled_count)
					{
						unsigned int n1 = n % 
							(slot->task->already_unrolled_count * slot->task->period);
						unsigned int suffix = n1 / slot->task->period;

						sprintf (task_name + strlen (task_name), "_%d", suffix);
					}

					if (!inline_p)
						cfprintf (indent, f, "extern void %s (void);\n", task_name);

					cfprintf (indent, f, "%s (); ", task_name);
					write_time_comment (f, slot->task->len);
					fprintf (f, "\n");
				}
			}
		}
		while (divider_count-- > 0)
			c_block_end (indent, f);

		if ((n == n_ticks-1) && (max_divider > 1))
		{
			cfprintf (indent, f, "%s_divider++;\n", prefix);
		}

		if (n_ticks > 1)
		{
			cfprintf (indent, f, "%s_function = %s_%d;\n",
				prefix, prefix, (n+1) % n_ticks);
		}

		cfprintf (indent, f, "%s", "");
		write_time_comment (f, tick_len);
		fprintf (f, "\n");

		if (tick->len >= 1.0)
			fprintf (stderr, "warning: tick %d takes too long\n", n);

		c_block_end (indent, f);
		fprintf (f, "\n");
	}

	/* For efficiency, the driver should be implemented as a single jump
	 * instruction.  We cannot guarantee that the C compiler will do
	 * this, so we hand-code it ourselves. */
	cfprintf (indent, f, " void %s_driver (void)\n{\n", prefix);
	cfprintf (indent, f, "#ifdef __m6809__\n");
	cfprintf (indent, f, "   asm (\"jmp\t[_%s_function]\");\n", prefix);
	cfprintf (indent, f, "#else\n");
	cfprintf (indent, f, "   (*%s_function) ();\n", prefix);
	cfprintf (indent, f, "#endif\n");
	cfprintf (indent, f, "}\n\n");

	/* Generate the initialization function. */

	cfprintf (indent, f, "void %s_init (void)\n{\n", prefix);
	cfprintf (indent, f, "   %s_function = %s_0;\n", prefix, prefix);
	cfprintf (indent, f, "   %s_divider = 0;\n", prefix);
	cfprintf (indent, f, "}\n\n");
}


void init_schedule (void)
{
	n_ticks = 0;
	n_tasks = 0;
}


/**
 * Expand the tick table to a width of 'new_tick_count'.
 */
void expand_ticks (unsigned int new_tick_count)
{
	unsigned int tickno;

	/* Don't allow infinite expansion */
	if (new_tick_count > max_ticks)
		new_tick_count = max_ticks;

	/* TODO : we are hard unrolling this 8 times now.
	This function will never be called more than once.
	Multiple expansions would require changing the code below
	to initialize only the new ticks. */
	n_ticks = 8;

	/* Initialize the new ticks */
	for (tickno = 0; tickno < n_ticks ; tickno++)
	{
		ticks[tickno].n_slots = 0;
		ticks[tickno].len = 0.0;
	}
}


/**
 * Find the best starting bucket to put a task with PERIOD and length LEN.
 * COUNT such buckets will have a slot added.
 *
 * We search through all possible starting buckets and choose the one that
 * is best.
 */
unsigned int find_best_tick (unsigned int period, unsigned int count, double len)
{
	unsigned int tickno, best = 0;
	double best_len = 99999.0;
	unsigned int index;

	/* printf ("find_best_tick: period %d count %d len %g\n", period, count, len);
	printf ("checking %d different starting points\n", n_ticks / count);
	printf ("adding %d different ticks for each\n", count); */

	/* The outermost loop iterates all possible results. */
	for (tickno = 0; tickno < n_ticks / count; tickno++)
	{
		double total_len = 0.0;

		/* Given a choice for the starting bucket, the innermost loop calculates
		the total amount of work being done by that bucket and all of the
		other buckets that would be used for this task (if count > 1).
		TOTAL_LEN represents the total amount of work already being done in
		these ticks. */

		for (index = 0; index < count; index++)
		{
			/* See how much work this tick is already doing now. */
			double candidate_len = ticks[tickno + (n_ticks / count) * index].len;

			/* If adding this task here would cause an overflow (i.e.
			all tasks would take longer than 1 tick to finish), then
			set the cost very high, disparaging this choice. */
			if (candidate_len + len >= 1.0)
				candidate_len = 99999.0;

			/* If the period is larger than the number of ticks (i.e.
			there is a divider here), prefer the last tick for this.
			It is best if all dividers share the same bucket. */
			else if ((period > n_ticks) && (tickno == n_ticks/count - 1))
				candidate_len = -1.0;

			total_len += candidate_len;
		}

		/* Track which of the alternatives is least utilized. */
		if (total_len < best_len)
		{
			best_len = total_len;
			best = tickno;
		}
	}

	/* printf ("best is %d, best_len = %g\n\n", best, best_len); */
	return best;
}


/**
 * Allocate a slot in a particular tick for a task to
 * be entered.
 */
struct slot *alloc_slot (unsigned int tickno)
{
	struct tick *tick = &ticks[tickno];

	if (tick->n_slots+1 == MAX_SLOTS_PER_TICK)
	{
		fprintf (stderr, "error: too many tasks scheduled in same tick\n");
		fprintf (stderr, "Please increase MAX_SLOTS_PER_TICK and rebuild scheduler\n");
		exit (1);
	}
	return &tick->slots[tick->n_slots++];
}


/**
 * Add a new task to the schedule.
 */
void add_task (char *name, unsigned int period, double len)
{
	unsigned int count, base;
	struct slot *slot;
	struct task *task;
	unsigned int divider = 1;
	char *end;
	unsigned int already_unrolled_count = 0;
	char *c;

	/* Is this entry dependent on a conditional? */
	if ((c = strchr (name, '?')) != NULL)
	{
		int cond;
		for (cond = 0; cond < n_conditionals; cond++)
		{
			if (!strcmp (conditionals[cond], c+1))
			{
				/* The conditional exists.  Proceed, and strip off
				the conditional part of the expression. */
				*c = '\0';
				goto conditional_defined;
			}
		}
		/* The conditional is not defined.  Do not define this task. */
		fprintf (stderr, "warning: skipping entry for '%s'\n", name);
		return;
	}
conditional_defined:;

	/* Support names of the form <function>/<divider>.
	This means that the function has already been unrolled. */

	end = name + strlen (name) - 2;
	if (*end == '/')
	{
		already_unrolled_count = end[1] - '0';
		*end = '\0';
	}

	/* Fill in the task structure */
	task = &tasks[n_tasks++];
	strcpy (task->name, name);
	task->period = period;
	task->len = len;
	task->already_unrolled_count = already_unrolled_count;
	task->n_slots = 0;

	/* Figure out how many slots this task should be assigned to.
	 *
	 * If the periodicity is greater than the number of times
	 * the interrupt handler has been unrolled so far, then it
	 * may need to be unrolled further.  Otherwise, it can be
	 * scheduled into the existing set of ticks. */

	if (period > n_ticks)
	{
		/* Unrolling is limited by 'max_ticks'.  If a task
		wants to run less frequently than this, then it is
		scheduled into exactly 1 tick, and the call is bracketed
		by an 'if' check that will only call it a certain
		percentage of the time. */
		if (period <= max_ticks)
		{
			/* Try to expand the tick table.
			This may change the value of 'n_ticks'. */
			expand_ticks (period);

			count = n_ticks / period;
		}
		else
		{
			/* Calculate the divider for a condition call.
			For example, if there are 8 interrupt handlers but
			the period is every 16 ticks, then in one tick,
			the task will be called but only 1 out of 2 times. */
			divider = period / n_ticks;

			/* The divider is tracked via a free-running global
			variable at runtime.  As it may overflow, don't
			permit extremely large periods. */
			if (divider > max_divider)
			{
				max_divider = divider;
				if (max_divider >= 256)
				{
					fprintf (stderr, "error: period too large\n");
					exit (1);
				}
			}
			count = 1;
		}
	}
	else
	{
		count = n_ticks / period;
	}

	/* The number of times the task must be scheduled is now
	known.  Now choose which instances of the interrupt handler
	it will go into.  BASE will be a value between 0 and n_ticks,
	which says which is the first tick to be used.  If the task
	is scheduled multiple times, it will be spread evenly
	across all ticks. */

	base = find_best_tick (period, count, len);

	/* Create the slots (calls) */

	while (count > 0)
	{
		slot = alloc_slot (base);
		slot->divider = divider;
		slot->task = task;
		task->n_slots++;

		/* Update the running count of how much time is
		spent running this tick, on average. */
		ticks[base].len += (task->len / divider);

		/* Move to the next tick, spreading evenly. */
		base = (base + period) % n_ticks;
		count--;
	}
}


/**
 * Parse a time string.  The value can be given in ticks or cycles.
 */
double parse_time (const char *string)
{
	const char *suffix = string + strlen (string) - 1;
	switch (*suffix)
	{
		case 'c':
		case 'C':
			return strtod (string, NULL) / (1.0 * cycles_per_interrupt);

		default:	
			return strtod (string, NULL);
	}
}


/**
 * Parse an input schedule file.
 */
void parse_schedule (FILE *f)
{
	char *name;
	unsigned int period;
	float len;
	char line[512];
	int lineno = 0;
	const char *delims = " \t\n";

	for (;;)
	{
		fgets (line, 511, f);
		if (feof (f))
			break;
		lineno++;
#if 0
		fprintf (stderr, "<<%03d>>  %s", lineno, line);
#endif

		name = strtok (line, delims);
		if (!name || *name == '#')
			continue;

		period = (unsigned int)parse_time (strtok (NULL, delims));
		if (period & (period - 1))
		{
			fprintf (stderr, 
				"error: invalid period '%d' for '%s' (must be power of 2)\n", 
				period, name);
			exit (1);
		}

		len = parse_time (strtok (NULL, delims));
		if (len >= period)
		{
			fprintf (stderr, 
				"error: '%s' has length greater than its period\n", name);
			exit (1);
		}

		add_task (name, period, len);
	}
}


int main (int argc, char *argv[])
{
	unsigned int argn;
	FILE *outfile = stdout;

	init_schedule ();

	argn = 1;
	while (argn < argc)
	{
		if (argv[argn][0] == '-')
		{
			switch (argv[argn++][1])
			{
				case 'o':
					outfile = fopen (argv[argn], "w");
					break;

				case 'i':
					strcpy (include_files[n_includes++].name, argv[argn]);
					break;

				case 'M':
					max_ticks = strtoul (argv[argn], NULL, 0);
					break;

				case 'p':
					prefix = argv[argn];
					break;

				/* TODO - add option to add a task directly from the
				command-line, without requiring a file; this would make it
				easier to generate the task list on-the-fly, for making
				certain things optional */

				case 'D':
					conditionals[n_conditionals++] = argv[argn];
					break;
			}
		}
		else
		{
			FILE *infile = fopen (argv[argn], "r");
			parse_schedule (infile);
			fclose (infile);
		}
		argn++;
	}

	write_tick_driver (outfile);
	if (outfile != stdout)
		fclose (outfile);
	return 0;
}

