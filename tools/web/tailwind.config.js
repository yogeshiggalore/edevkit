/** @type {import('tailwindcss').Config} */
module.exports = {
  darkMode: 'class',
  content: [
    './templates/**/*.html',
    './static/script.js',
    './pages/**/*.html',
  ],
  theme: {
    extend: {
      fontFamily: {
        sans: ['Inter', 'ui-sans-serif', 'system-ui', '-apple-system', 'sans-serif'],
        mono: ['"JetBrains Mono"', 'ui-monospace', 'SFMono-Regular', 'Menlo', 'monospace'],
      },
      colors: {
        brand: {
          50: '#ecfeff', 100: '#cffafe', 200: '#a5f3fc', 300: '#67e8f9',
          400: '#22d3ee', 500: '#06b6d4', 600: '#0891b2', 700: '#0e7490',
          800: '#155e75', 900: '#164e63', 950: '#083344',
        },
        ink: {
          50: '#f8fafc', 100: '#f1f5f9', 200: '#e2e8f0', 300: '#cbd5e1',
          400: '#94a3b8', 500: '#64748b', 600: '#475569', 700: '#334155',
          800: '#1e293b', 900: '#0f172a', 950: '#080d1a',
        },
      },
      animation: {
        'fade-up':     'fade-up 0.7s cubic-bezier(.16,1,.3,1) both',
        'fade-in':     'fade-in 0.5s cubic-bezier(.16,1,.3,1) both',
        'draw-pulse':  'draw-pulse 1.6s cubic-bezier(.16,1,.3,1) 0.2s forwards',
        'pulse-soft':  'pulse-soft 2.4s cubic-bezier(.4,0,.6,1) infinite',
        'shimmer':     'shimmer 2s linear infinite',
        'gradient-x':  'gradient-x 8s ease infinite',
      },
      keyframes: {
        'fade-up':    { '0%': { opacity: '0', transform: 'translateY(10px)' },
                        '100%': { opacity: '1', transform: 'translateY(0)' } },
        'fade-in':    { '0%': { opacity: '0' }, '100%': { opacity: '1' } },
        'draw-pulse': { '0%': { strokeDashoffset: '1200' },
                        '100%': { strokeDashoffset: '0' } },
        'pulse-soft': { '0%, 100%': { opacity: '1', transform: 'scale(1)' },
                        '50%':      { opacity: '.55', transform: 'scale(1.3)' } },
        'shimmer':    { '0%': { backgroundPosition: '-200% 0' },
                        '100%': { backgroundPosition: '200% 0' } },
        'gradient-x': { '0%, 100%': { backgroundPosition: '0% 50%' },
                        '50%':      { backgroundPosition: '100% 50%' } },
      },
    },
  },
  plugins: [
    require('@tailwindcss/typography'),
    require('@tailwindcss/forms'),
  ],
};
