module.exports = {
  dependency: {
    platforms: {
      ios: {},
      android: {
        sourceDir: './android',
        packageImportPath: 'import com.danielkpl2.webmalphaplayer.RNWebMAlphaPlayerPackage;',
        packageInstance: 'new RNWebMAlphaPlayerPackage()',
      },
    },
  },
};
