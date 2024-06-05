# Scripts

# Python wrapper

If enabled in cmake, thePythn3 wrapper will be built. It is a simplified wrapper built around the Agent class. It is not meant to be a full-featured wrapper, but rather a simple way to interact with the Agent class from Python.

To build the wrapper, be sure to enable it by setting the `MADS_ENABLE_PYTHON_WRAPPER` option to `ON` in cmake configuration step. Then, build the project as usual **and install it**. The wrapper will be built as a low-level shared library, `_Miroscic.so`, and as a high-level interface module, `Miroscic.py`. The latter is typically imported in Python scripts to interact with the Agent class.

See the `agent.py` script for an example of how to use the Python wrapper.