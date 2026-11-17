const examples = {
  rocket: { name: 'Rocket League', total: '2,481<span>.3h</span>', accounts: [['E','Epic Main','1,520.6h','epic'],['E','Epic Alt','610.2h','epic'],['S','Steam','350.5h','steam']] },
  cs: { name: 'Counter-Strike 2', total: '1,354<span>.1h</span>', accounts: [['S','Steam Main','1,354.1h','steam']] },
  apex: { name: 'Apex Legends', total: '627<span>.1h</span>', accounts: [['E','EA Main','501.7h','epic'],['E','EA Alt','125.4h','epic']] }
};
const title = document.querySelector('#detail-name');
const total = document.querySelector('#detail-total');
const badge = document.querySelector('#detail-badge');
const rows = document.querySelector('#account-rows');
const heading = document.querySelector('#dashboard-heading');
document.querySelectorAll('[data-game]').forEach(button => {
  button.addEventListener('click', () => {
    document.querySelectorAll('[data-game]').forEach(el => el.classList.toggle('active', el === button));
    const game = examples[button.dataset.game];
    title.textContent = game.name;
    total.innerHTML = game.total;
    badge.textContent = `${game.accounts.length} ${game.accounts.length === 1 ? 'ACCOUNT' : 'ACCOUNTS'}`;
    rows.replaceChildren(...game.accounts.map(([initial,name,hours,platform]) => {
      const row = document.createElement('div'); row.className = 'account-row';
      const label = document.createElement('span');
      const icon = document.createElement('i'); icon.className = `account-mark ${platform}`; icon.textContent = initial;
      label.append(icon, document.createTextNode(' ' + name));
      const value = document.createElement('strong'); value.textContent = hours;
      row.append(label, value); return row;
    }));
  });
});
const headings = {dashboard:'Your dashboard', games:'Your game library', sessions:'Your play sessions', accounts:'Your accounts'};
document.querySelectorAll('[data-view]').forEach(button => button.addEventListener('click', () => {
  document.querySelectorAll('[data-view]').forEach(el => el.classList.toggle('selected', el === button));
  heading.textContent = headings[button.dataset.view];
}));
const menuButton = document.querySelector('.menu-button');
const nav = document.querySelector('.primary-nav');
menuButton.addEventListener('click', () => { const open = menuButton.getAttribute('aria-expanded') !== 'true'; menuButton.setAttribute('aria-expanded',String(open)); nav.classList.toggle('is-open',open); });
nav.querySelectorAll('a').forEach(a => a.addEventListener('click', () => {menuButton.setAttribute('aria-expanded','false'); nav.classList.remove('is-open');}));
document.querySelector('#year').textContent = String(new Date().getFullYear());
