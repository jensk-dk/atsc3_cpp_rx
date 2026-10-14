module.exports = {
  root: true,
  env: { browser: true, es2020: true },
  parser: 'vue-eslint-parser',
  parserOptions: { parser: '@typescript-eslint/parser', sourceType: 'module' },
  extends: ['eslint:recommended', 'plugin:vue/vue3-recommended', 'plugin:@typescript-eslint/recommended', 'prettier'],
}
