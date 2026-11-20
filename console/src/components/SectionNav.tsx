import { NavLink } from 'react-router-dom';
import type { NavItem } from '../app/navigation';

/** A row of links. The router marks the active one with the `active` class. */
export function SectionNav({ ariaLabel, items, className }: { ariaLabel: string; items: readonly NavItem[]; className: string }) {
  return (
    <nav className={className} aria-label={ariaLabel}>
      {items.map((item) => (
        <NavLink key={item.to} to={item.to} end={item.end} className={({ isActive }) => (isActive ? 'active' : undefined)}>
          {item.label}
        </NavLink>
      ))}
    </nav>
  );
}
