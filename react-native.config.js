module.exports = {
  dependency: {
    platforms: {
      ios: {},
      android: {
        sourceDir: './react-native/android',
        packageImportPath: 'import dev.mediacache.alphavideo.reactnative.AlphaVideoPackage;',
        packageInstance: 'new AlphaVideoPackage()',
      },
    },
  },
};
