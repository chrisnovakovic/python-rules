// Package preamble is the Go interface to the .pex preamble format.
package preamble

// ConfigPath is the zip file member within the .pex archive containing the .pex preamble
// configuration. It is typically the first member of the archive (for performance reasons), but
// does not necessarily have to be.
const ConfigPath = ".bootstrap/PLZ_PREAMBLE_CONFIG"

// Config represents a .pex preamble configuration.
type Config struct {
	// Interpreters is a list of relative or absolute paths to Python interpreters that the preamble
	// should attempt to invoke in the order in which they are given. The preamble duplicates the
	// actions of the shell when attempting to locate an interpreter with a relative path that does not
	// contain a forward slash.
	Interpreters []string `json:"interpreters"`

	// InterpreterArgs is a list of command line arguments that the preamble should pass to the Python
	// interpreters when attempting to invoke them.
	InterpreterArgs []string `json:"interpreter_args"`
}
