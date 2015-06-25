# This makefile does nothing but delegating the actual compilation to build.py.

all:
	@python brewery.py build

clean:
	@python brewery.py clean

reallyclean:
	@python brewery.py reallyclean

test:
	@python brewery.py test

lint:
	@find caffe2 -type f -exec python cpplint.py {} \;

linecount:
	@cloc --read-lang-def=caffe.cloc caffe2 pycaffe2 || \
		echo "Cloc is not available on the machine. You can install cloc with " && \
		echo "    sudo apt-get install cloc"