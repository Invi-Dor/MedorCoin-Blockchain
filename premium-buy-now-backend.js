// Premium Buy Now backend logic (UI flow and navigation)
(function(){
  const billingRadios = document.querySelectorAll('input[name="billing"]');
  const methodRadios = document.querySelectorAll('input[name="method"]');
  const checkoutBtn = document.getElementById('checkoutBtn');
  const modalBackdrop = document.getElementById('modal-backdrop');
  const modalYes = document.getElementById('modalYes');
  const modalNo = document.getElementById('modalNo');

  function updateCheckoutState() {
    const billingSelected = document.querySelector('input[name="billing"]:checked');
    const methodSelected = document.querySelector('input[name="method"]:checked');
    checkoutBtn.disabled = !(billingSelected && methodSelected);
  }

  billingRadios.forEach(r => r.addEventListener('change', updateCheckoutState));
  methodRadios.forEach(r => r.addEventListener('change', updateCheckoutState));
  checkoutBtn.addEventListener('click', () => {
    modalBackdrop.style.display = 'block';
  });

  modalNo.addEventListener('click', () => {
    modalBackdrop.style.display = 'none';
  });

  modalYes.addEventListener('click', () => {
    modalBackdrop.style.display = 'none';
    const billing = document.querySelector('input[name="billing"]:checked')?.value;
    const method = document.querySelector('input[name="method"]:checked')?.value;
    const address = document.querySelector('input[name="method"]:checked')?.dataset?.address;

    const price = billing === 'monthly' ? 45 : 297;
    const url = new URL(window.location.origin + '/invoice.html');
    url.searchParams.set('product', 'premium');
    url.searchParams.set('plan', billing);
    url.searchParams.set('price', price);
    url.searchParams.set('method', method);
    url.searchParams.set('address', address);
    window.location.href = url.toString();
  });

  updateCheckoutState();
})();
