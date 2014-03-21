webidl_files += 
generated_events_webidl_files += 
test_webidl_files += 
preprocessed_test_webidl_files += 
generated_webidl_files += 
preprocessed_webidl_files += 

# We build files in 'unified' mode by including several files
# together into a single source file.  This cuts down on
# compilation times and debug information size.  16 was chosen as
# a reasonable compromise between clobber rebuild time, incremental
# rebuild time, and compiler memory usage.
unified_binding_cpp_files := 

# Make sometimes gets confused between "foo" and "$(CURDIR)/foo".
# Help it out by explicitly specifiying dependencies.
all_absolute_unified_files := \
  $(addprefix $(CURDIR)/,$(unified_binding_cpp_files))
$(all_absolute_unified_files): $(CURDIR)/%: %
