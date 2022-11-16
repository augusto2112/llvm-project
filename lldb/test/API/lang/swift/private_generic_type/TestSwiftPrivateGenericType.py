import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *
import lldbsuite.test.lldbutil as lldbutil

class TestSwiftPrivateGenericType(TestBase):

    def setUp(self):
        TestBase.setUp(self)

    @skipUnlessDarwin
    @swiftTest
    def test_private_generic_type(self):
        """Test a library with a private import for which there is no debug info"""
        invisible_swift = self.getBuildArtifact("Private.swift")
        import shutil
        shutil.copyfile("Private.swift", invisible_swift)
        self.build()
        os.unlink(invisible_swift)
        os.unlink(self.getBuildArtifact("Private.swiftmodule"))
        os.unlink(self.getBuildArtifact("Private.swiftinterface"))

        self.runCmd("settings set target.experimental.swift-enable-generic-expr-evaluation true")
        self.runCmd('settings set symbols.swift-validate-typesystem false')
        self.runCmd('log enable lldb expr')
        lldbutil.run_to_source_breakpoint(
            self, 'break here', lldb.SBFileSpec('Public.swift'),
            extra_images=['Public'])
        self.expect("p  self", substrs=["Wrapper<T>", "iniivisible man"])
