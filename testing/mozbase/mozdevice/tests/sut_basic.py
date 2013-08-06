from sut import MockAgent
import mozdevice
import mozlog
import unittest

class BasicTest(unittest.TestCase):

    def test_init(self):
        """Tests DeviceManager initialization."""
        a = MockAgent(self)

        d = mozdevice.DroidSUT("127.0.0.1", port=a.port, logLevel=mozlog.DEBUG)
        # all testing done in device's constructor
        a.wait()

    def test_init_err(self):
        """Tests error handling during initialization."""
        cmds = [("testroot", "/mnt/sdcard"),
                ("isdir /mnt/sdcard/tests", "/mnt/sdcard/tests: No such file or directory\n"),
                ("isdir /mnt/sdcard/tests", "/mnt/sdcard/tests: No such file or directory\n"),
                ("mkdr /mnt/sdcard/tests", "/mnt/sdcard/tests successfully created"),
                ("ver", "SUTAgentAndroid Version 1.14")]
        a = MockAgent(self, start_commands = cmds)
        dm = mozdevice.DroidSUT("127.0.0.1", port=a.port, logLevel=mozlog.DEBUG)
        a.wait()

    def test_timeout_normal(self):
        """Tests DeviceManager timeout, normal case."""
        a = MockAgent(self, commands = [("isdir /mnt/sdcard/tests", "TRUE"),
                                        ("cd /mnt/sdcard/tests", ""),
                                        ("ls", "test.txt"),
                                        ("rm /mnt/sdcard/tests/test.txt",
                                         "Removed the file")])
        d = mozdevice.DroidSUT("127.0.0.1", port=a.port, logLevel=mozlog.DEBUG)
        ret = d.removeFile('/mnt/sdcard/tests/test.txt')
        self.assertEqual(ret, None) # if we didn't throw an exception, we're ok
        a.wait()

    def test_timeout_timeout(self):
        """Tests DeviceManager timeout, timeout case."""
        a = MockAgent(self, commands = [("isdir /mnt/sdcard/tests", "TRUE"),
                                        ("cd /mnt/sdcard/tests", ""),
                                        ("ls", "test.txt"),
                                        ("rm /mnt/sdcard/tests/test.txt", 0)])
        d = mozdevice.DroidSUT("127.0.0.1", port=a.port, logLevel=mozlog.DEBUG)
        d.default_timeout = 1
        exceptionThrown = False
        try:
            d.removeFile('/mnt/sdcard/tests/test.txt')
        except mozdevice.DMError:
            exceptionThrown = True
        self.assertEqual(exceptionThrown, True)
        a.wait()

    def test_shell(self):
        """Tests shell command"""
        for cmd in [ ("exec foobar", False), ("execsu foobar", True) ]:
            for retcode in [ 1, 2 ]:
                a = MockAgent(self, commands=[(cmd[0],
                                               "\nreturn code [%s]" % retcode)])
                d = mozdevice.DroidSUT("127.0.0.1", port=a.port)
                exceptionThrown = False
                try:
                    d.shellCheckOutput(["foobar"], root=cmd[1])
                except mozdevice.DMError:
                    exceptionThrown = True
                expectedException = (retcode != 0)
                self.assertEqual(exceptionThrown, expectedException)

                a.wait()

if __name__ == '__main__':
    unittest.main()
