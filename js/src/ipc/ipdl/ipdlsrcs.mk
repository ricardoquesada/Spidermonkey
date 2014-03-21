ALL_IPDLSRCS := 

# We build files in 'unified' mode by including several files
# together into a single source file.  This cuts down on
# compilation times and debug information size.  16 was chosen as
# a reasonable compromise between clobber rebuild time, incremental
# rebuild time, and compiler memory usage.
CPPSRCS := 

# Make sometimes gets confused between "foo" and "$(CURDIR)/foo".
# Help it out by explicitly specifiying dependencies.
all_absolute_unified_files := \
  $(addprefix $(CURDIR)/,$(CPPSRCS))
$(all_absolute_unified_files): $(CURDIR)/%: %
IPDLDIRS := 
