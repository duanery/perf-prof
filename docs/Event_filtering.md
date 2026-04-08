5. Event filtering
==================

Trace events can be filtered in the kernel by associating boolean
'filter expressions' with them.  As soon as an event is logged into
the trace buffer, its fields are checked against the filter expression
associated with that event type.  An event with field values that
'match' the filter will appear in the trace output, and an event whose
values don't match will be discarded.  An event with no filter
associated with it matches everything, and is the default when no
filter has been set for an event.

5.1 Expression syntax
---------------------

A filter expression consists of one or more 'predicates' that can be
combined using the logical operators '&&' and '||'.  A predicate is
simply a clause that compares the value of a field contained within a
logged event with a constant value and returns either 0 or 1 depending
on whether the field value matched (1) or didn't match (0)::

	  field-name relational-operator value

Parentheses can be used to provide arbitrary logical groupings and
double-quotes can be used to prevent the shell from interpreting
operators as shell metacharacters.

The field-names available for use in filters can be found in the
'format' files for trace events (see section 4).

The relational-operators depend on the type of the field being tested:

The operators available for numeric fields are:

==, !=, <, <=, >, >=, &

And for string fields they are:

==, !=, ~

The glob (~) accepts a wild card character (\*,?) and character classes
([). For example::

  prev_comm ~ "*sh"
  prev_comm ~ "sh*"
  prev_comm ~ "*sh*"
  prev_comm ~ "ba*sh"

If the field is a pointer that points into user space (for example
"filename" from sys_enter_openat), then you have to append ".ustring" to the
field name::

  filename.ustring ~ "password"

As the kernel will have to know how to retrieve the memory that the pointer
is at from user space.

You can convert any long type to a function address and search by function name::

  call_site.function == security_prepare_creds

The above will filter when the field "call_site" falls on the address within
"security_prepare_creds". That is, it will compare the value of "call_site" and
the filter will return true if it is greater than or equal to the start of
the function "security_prepare_creds" and less than the end of that function.

The ".function" postfix can only be attached to values of size long, and can only
be compared with "==" or "!=".

Cpumask fields or scalar fields that encode a CPU number can be filtered using
a user-provided cpumask in cpulist format. The format is as follows::

  CPUS{$cpulist}

Operators available to cpumask filtering are:

& (intersection), ==, !=

For example, this will filter events that have their .target_cpu field present
in the given cpumask::

  target_cpu & CPUS{17-42}
